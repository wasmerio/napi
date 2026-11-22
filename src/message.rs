//! Ownership and memory of the messages a context's environments send each
//! other (`unofficial_napi_message_*`, behind `postMessage`).
//!
//! The provider's message ids are process-wide, because the sender and the
//! receiver are different environments, possibly in different stores of the
//! context. A guest may only take or drop a message its own context created:
//! every id is checked against the context's [`PendingMessages`] first, so a
//! guest cannot consume another application's message by guessing its id.
//!
//! The provider charges a message to its sender's budget
//! ([`crate::Pool::SerializedMessage`]) from before it allocates it until it
//! is taken or dropped. The context frees every message still queued when it
//! stops ([`crate::NapiRuntimeControl::terminate_all`]) or is dropped.
//!
//! A message can hold `SharedArrayBuffer`s whose bytes are in the sender's
//! guest memory (the guest heap backs V8's array buffers). The queue then
//! holds a lease on that memory ([`MessageMemory`]), and only an environment
//! on the same memory may take the message: the mapping belongs to the
//! sender's stores, and a receiver on another memory could not address it.

use std::{
    collections::HashMap,
    sync::{Arc, Mutex},
};

#[cfg(not(all(target_arch = "wasm32", feature = "js")))]
use std::sync::Weak;

#[cfg(not(all(target_arch = "wasm32", feature = "js")))]
use crate::guest_heap::GuestHeap;
use crate::snapi::snapi_bridge_unofficial_message_drop as drop_provider_message;

/// The guest memory a queued message's `SharedArrayBuffer`s point into.
#[cfg(not(all(target_arch = "wasm32", feature = "js")))]
pub(crate) enum MessageMemory {
    /// A shared memory (an instance and its WASIX threads): the heap, whose
    /// allocator the backing stores return their blocks to, and a detached
    /// handle that keeps the mapping alive after every store using it is
    /// gone.
    Shared {
        heap: Arc<GuestHeap>,
        _mapping: wasmer::SharedMemory,
    },
    /// A memory only its own store uses: only that store can take the
    /// message, and while it can, it keeps the mapping. Held weakly, so the
    /// queue never keeps a heap past its store.
    Owned { heap: Weak<GuestHeap> },
}

/// A message of this context that was not taken or dropped yet.
#[derive(Default)]
pub(crate) struct QueuedMessage {
    #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
    memory: Option<MessageMemory>,
}

impl std::fmt::Debug for QueuedMessage {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("QueuedMessage").finish_non_exhaustive()
    }
}

impl QueuedMessage {
    /// A message whose `SharedArrayBuffer`s (if any) point into `memory`.
    #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
    pub(crate) fn in_memory(memory: Option<MessageMemory>) -> Self {
        Self { memory }
    }

    /// Whether an environment on `heap` (its store's guest heap, if any) can
    /// receive the message: always, unless it points into a guest memory,
    /// which must then be the receiver's.
    #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
    pub(crate) fn readable_by(&self, heap: Option<&Arc<GuestHeap>>) -> bool {
        match &self.memory {
            None => true,
            Some(MessageMemory::Shared { heap: sender, .. }) => {
                heap.is_some_and(|heap| Arc::ptr_eq(heap, sender))
            }
            Some(MessageMemory::Owned { heap: sender }) => heap.is_some_and(|heap| {
                sender.strong_count() > 0 && std::ptr::eq(sender.as_ptr(), Arc::as_ptr(heap))
            }),
        }
    }
}

/// The messages a context created that were not taken or dropped yet.
#[derive(Debug, Default)]
pub(crate) struct PendingMessages {
    /// `None` once the context stopped: no message is accepted anymore.
    messages: Mutex<Option<HashMap<u32, QueuedMessage>>>,
}

impl PendingMessages {
    pub(crate) fn new() -> Arc<Self> {
        Arc::new(Self {
            messages: Mutex::new(Some(HashMap::new())),
        })
    }

    /// Records a message the provider just created as this context's. If the
    /// context stopped meanwhile, the message is freed instead and `false`
    /// returned.
    pub(crate) fn insert(&self, id: u32, message: QueuedMessage) -> bool {
        let mut messages = self.messages.lock().expect("poisoned message registry");
        match messages.as_mut() {
            Some(messages) if id != 0 && !messages.contains_key(&id) => {
                messages.insert(id, message);
                true
            }
            _ => {
                drop(messages);
                // SAFETY: the provider just created `id` for this caller, and
                // no one else knows it. It is freed before its memory lease.
                #[allow(unused_unsafe)]
                unsafe {
                    drop_provider_message(id)
                };
                drop(message);
                false
            }
        }
    }

    /// Removes one of this context's messages, for the caller to take or drop
    /// in the provider before dropping the returned entry (which may hold the
    /// lease on the memory the message points into). `None` for an id this
    /// context does not own (or no longer owns).
    pub(crate) fn remove(&self, id: u32) -> Option<QueuedMessage> {
        self.messages
            .lock()
            .expect("poisoned message registry")
            .as_mut()?
            .remove(&id)
    }

    /// Frees every queued message and refuses new ones: the context's
    /// JavaScript will never run again.
    pub(crate) fn close(&self) {
        let messages = self
            .messages
            .lock()
            .expect("poisoned message registry")
            .take();
        for (id, message) in messages.into_iter().flatten() {
            // SAFETY: the registry owned `id`, and removing it under the lock
            // gave it to this call alone. The provider message, whose backing
            // stores may point into the leased memory, goes first.
            #[allow(unused_unsafe)]
            unsafe {
                drop_provider_message(id)
            };
            drop(message);
        }
    }
}

impl Drop for PendingMessages {
    fn drop(&mut self) {
        self.close();
    }
}

#[cfg(all(test, not(target_arch = "wasm32")))]
mod tests {
    use std::sync::Arc;

    use wasmer::{AsStoreMut, Memory, MemoryType, Pages, Store};

    use super::{MessageMemory, QueuedMessage};
    use crate::{budget::ResourceBudget, guest_heap::GuestHeap};

    fn heap(store: &mut Store, shared: bool) -> (Memory, Arc<GuestHeap>) {
        let memory = Memory::new(
            &mut *store,
            MemoryType::new(Pages(16), Some(Pages(64)), shared),
        )
        .unwrap();
        let heap = GuestHeap::get_or_create(
            &mut store.as_store_mut(),
            &memory,
            ResourceBudget::unlimited(),
            None,
        )
        .expect("guest heap");
        (memory, heap)
    }

    #[test]
    fn a_message_without_guest_memory_is_readable_anywhere() {
        let mut store = Store::default();
        let (_memory, heap) = heap(&mut store, false);
        let message = QueuedMessage::default();
        assert!(message.readable_by(None));
        assert!(message.readable_by(Some(&heap)));
    }

    #[test]
    fn a_message_in_a_memory_is_readable_only_on_that_memory() {
        let mut store = Store::default();
        let (memory, sender) = heap(&mut store, true);
        let (_other_memory, other) = heap(&mut store, true);
        let message = QueuedMessage::in_memory(Some(MessageMemory::Shared {
            heap: Arc::clone(&sender),
            _mapping: memory.as_shared(&store).expect("a shared memory"),
        }));
        assert!(message.readable_by(Some(&sender)));
        assert!(!message.readable_by(Some(&other)));
        assert!(!message.readable_by(None));

        let (_owned_memory, owned) = heap(&mut store, false);
        let message = QueuedMessage::in_memory(Some(MessageMemory::Owned {
            heap: Arc::downgrade(&owned),
        }));
        assert!(message.readable_by(Some(&owned)));
        assert!(!message.readable_by(Some(&other)));
        drop(owned);
        // The sender's store is gone: nothing can read it anymore.
        assert!(!message.readable_by(Some(&other)));
    }
}
