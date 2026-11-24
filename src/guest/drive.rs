//! Runs a guest call to completion on the thread that makes it.
//!
//! JavaScript calls a guest function from inside V8's frames, which cannot be
//! suspended: the function has to have returned when the trampoline does. A
//! guest of an async runtime may have to wait in the middle of it, though.
//! Every syscall of a WASIX v2 guest is an async import, and one that cannot
//! finish at once (a file read handed to a blocking pool, a timer, a socket)
//! makes the engine trap a synchronous call. So the trampoline makes the call
//! an async one and stays in it: it polls the call, and while the call waits,
//! the thread sleeps.
//!
//! What the call waits for must therefore be completed by another thread: the
//! runtime's blocking pool, its timers, its I/O driver. Work that only this
//! thread's executor can run does not progress while the thread sleeps here.
//!
//! The call is polled from inside the poll of the task that runs the guest,
//! which never returns to its scheduler meanwhile. Tokio gives a task a budget
//! of operations between two returns and, once it is spent, has every tokio
//! future (a timer, a channel, the result of blocking work) answer "pending"
//! and wake its task at once, so that the task yields. Here nothing yields: a
//! callback that makes more syscalls than the budget would be woken and
//! refused for ever. With the `wasix` feature the call is therefore polled
//! outside tokio's budget.

use std::{
    cell::{Cell, RefCell},
    future::Future,
    mem::ManuallyDrop,
    pin::pin,
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
    },
    task::{Context, Poll, Wake, Waker},
    thread::{self, Thread},
};

/// Wakes the thread that sleeps in [`run_to_completion`].
struct Signal {
    thread: Thread,
    notified: AtomicBool,
}

impl Wake for Signal {
    fn wake(self: Arc<Self>) {
        self.wake_by_ref();
    }

    fn wake_by_ref(self: &Arc<Self>) {
        // The sleeper clears the flag before it polls, so a wake that finds
        // it set is one the sleeper has yet to see.
        if !self.notified.swap(true, Ordering::Release) {
            self.thread.unpark();
        }
    }
}

/// What [`run_to_completion`] keeps for its thread. One value, so that a
/// call looks its thread's up once.
struct Driver {
    /// How many [`run_to_completion`] calls are on this thread's stack.
    depth: Cell<usize>,
    /// One signal for each depth, made when the thread first gets that deep
    /// and kept until the thread ends.
    ///
    /// Calls nest: a guest function that JavaScript calls can call into
    /// JavaScript, which calls the guest again. Each waits on the signal of
    /// its own depth, so a wake meant for an outer call is still there when
    /// the inner one has returned.
    signals: RefCell<Vec<Arc<Signal>>>,
}

thread_local! {
    static DRIVER: Driver = const {
        Driver {
            depth: Cell::new(0),
            signals: RefCell::new(Vec::new()),
        }
    };
}

impl Driver {
    /// The signal of `depth`, as `Arc::as_ptr` gives it. The thread keeps
    /// its reference to the signal until it ends.
    fn signal(&self, depth: usize) -> *const Signal {
        let mut signals = self.signals.borrow_mut();
        if signals.len() <= depth {
            signals.resize_with(depth + 1, || {
                Arc::new(Signal {
                    thread: thread::current(),
                    notified: AtomicBool::new(false),
                })
            });
        }
        Arc::as_ptr(&signals[depth])
    }
}

/// Polls `future` on this thread until it is ready, sleeping while it waits.
pub(crate) fn run_to_completion<F: Future>(future: F) -> F::Output {
    struct Leave<'a>(&'a Cell<usize>);
    impl Drop for Leave<'_> {
        fn drop(&mut self) {
            self.0.set(self.0.get() - 1);
        }
    }

    // See the module's documentation. Harmless outside a tokio runtime.
    #[cfg(feature = "wasix")]
    let future = tokio::task::unconstrained(future);
    // Pinned here, not in the closure below, which would move it once more.
    let mut future = pin!(future);
    DRIVER.with(|driver| {
        let depth = driver.depth.replace(driver.depth.get() + 1);
        let _leave = Leave(&driver.depth);
        let signal = driver.signal(depth);
        // Nearly every call is ready at its first poll and wakes nobody, so
        // its waker borrows the thread's reference to the signal: one of its
        // own would cost each call four atomic updates. A clone of the
        // waker, which is what a future that waits keeps, takes its own.
        //
        // SAFETY: `signal` is what `Arc::as_ptr` gave for a reference the
        // thread keeps until it ends, which is after this call and after
        // `waker`. `ManuallyDrop` keeps `waker` from giving back a reference
        // it did not take.
        let waker = ManuallyDrop::new(Waker::from(unsafe { Arc::from_raw(signal) }));
        // SAFETY: as above.
        let signal = unsafe { &*signal };
        // A waker of an earlier call at this depth may still fire. That costs
        // this call a poll at most; it must not make it skip a sleep.
        signal.notified.store(false, Ordering::Release);
        let mut context = Context::from_waker(&waker);
        loop {
            if let Poll::Ready(output) = future.as_mut().poll(&mut context) {
                return output;
            }
            // `park` may return for no reason, and for the wake of another depth.
            while !signal.notified.swap(false, Ordering::Acquire) {
                thread::park();
            }
        }
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{
        sync::atomic::AtomicUsize,
        task::{Context, Poll},
        time::Duration,
    };

    /// Ready on the poll after another thread woke it.
    struct WokenElsewhere {
        polls: Arc<AtomicUsize>,
        ready: Arc<AtomicBool>,
        delay: Duration,
        started: bool,
    }

    impl Future for WokenElsewhere {
        type Output = usize;

        fn poll(mut self: std::pin::Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<usize> {
            let polls = self.polls.fetch_add(1, Ordering::SeqCst) + 1;
            if self.ready.load(Ordering::SeqCst) {
                return Poll::Ready(polls);
            }
            if !self.started {
                self.started = true;
                let (ready, waker, delay) = (self.ready.clone(), cx.waker().clone(), self.delay);
                thread::spawn(move || {
                    thread::sleep(delay);
                    ready.store(true, Ordering::SeqCst);
                    waker.wake();
                });
            }
            Poll::Pending
        }
    }

    fn woken_elsewhere(delay: Duration) -> WokenElsewhere {
        WokenElsewhere {
            polls: Arc::default(),
            ready: Arc::default(),
            delay,
            started: false,
        }
    }

    #[test]
    fn a_ready_future_returns_without_sleeping() {
        assert_eq!(run_to_completion(async { 7 }), 7);
    }

    #[test]
    fn sleeps_until_another_thread_wakes_it() {
        let polls = run_to_completion(woken_elsewhere(Duration::from_millis(20)));
        assert!(polls >= 2, "the first poll was pending");
    }

    #[test]
    fn an_inner_call_keeps_the_wake_of_the_outer_one() {
        // The outer future is woken while the inner call sleeps. The inner
        // call must not take that wake for its own and lose it.
        let outer_ready = Arc::new(AtomicBool::new(false));
        let mut started = false;
        let outer = std::future::poll_fn(|cx| {
            if outer_ready.load(Ordering::SeqCst) {
                return Poll::Ready(());
            }
            if !started {
                started = true;
                let (ready, waker) = (outer_ready.clone(), cx.waker().clone());
                thread::spawn(move || {
                    thread::sleep(Duration::from_millis(10));
                    ready.store(true, Ordering::SeqCst);
                    waker.wake();
                });
                // Still inside the outer poll, as a guest call nested in
                // JavaScript is.
                run_to_completion(woken_elsewhere(Duration::from_millis(60)));
                // Woken long ago: a lost wake would sleep forever below.
                if !outer_ready.swap(false, Ordering::SeqCst) {
                    panic!("the outer wake was due before the inner call returned");
                }
                let (ready, waker) = (outer_ready.clone(), cx.waker().clone());
                thread::spawn(move || {
                    thread::sleep(Duration::from_millis(10));
                    ready.store(true, Ordering::SeqCst);
                    waker.wake();
                });
            }
            Poll::Pending
        });
        run_to_completion(outer);
    }

    /// More operations than tokio allows a task between two returns to its
    /// scheduler, each finished by another thread.
    #[cfg(feature = "wasix")]
    #[test]
    fn a_call_inside_a_tokio_task_outlasts_the_task_budget() {
        let runtime = tokio::runtime::Builder::new_multi_thread()
            .worker_threads(1)
            .build()
            .unwrap();
        let (done, finished) = std::sync::mpsc::channel();
        thread::spawn(move || {
            let count = runtime.block_on(async {
                // As the trampoline does: from inside the task's own poll.
                run_to_completion(async {
                    let mut count = 0;
                    for _ in 0..1000 {
                        tokio::task::spawn_blocking(|| ()).await.unwrap();
                        count += 1;
                    }
                    count
                })
            });
            let _ = done.send(count);
        });
        let count = finished
            .recv_timeout(Duration::from_secs(60))
            .expect("the call never finished: the task's budget stopped it");
        assert_eq!(count, 1000);
    }

    #[test]
    fn a_stale_wake_does_not_end_a_later_call() {
        let stale = {
            let mut kept = None;
            run_to_completion(std::future::poll_fn(|cx| {
                kept = Some(cx.waker().clone());
                Poll::Ready(())
            }));
            kept.unwrap()
        };
        stale.wake();
        let polls = run_to_completion(woken_elsewhere(Duration::from_millis(20)));
        assert!(polls >= 2);
    }
}
