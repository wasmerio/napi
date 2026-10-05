//! Ownership and budget reservations for serialized cross-worker messages.
//!
//! The native bridge uses process-wide numeric handles because the producer
//! and consumer can be different V8 environments. A guest must never use a
//! handle minted by another NapiCtx, even when it guesses its numeric ID.

use std::{
    collections::HashMap,
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, Ordering},
    },
};

use crate::{
    budget::{OverBudget, Pool, ResourceBudget},
    snapi::snapi_bridge_unofficial_message_drop,
};

/// Temporary worst-case reservation covering serializer capacity, its object
/// table, and message metadata. The native serializer rejects a buffer above
/// 4 MiB and caps transfer side tables. On success the reservation shrinks to
/// the measured native bytes retained until the receiver consumes the handle.
pub(crate) const SERIALIZATION_RESERVATION: u64 = 32 * 1024 * 1024;

pub(crate) struct MessageCharge {
    budget: Arc<ResourceBudget>,
    bytes: u64,
    // Native serialized messages can retain SharedArrayBuffer backing in this
    // heap's guest memory. Keep its allocation metadata and WasmLinear charge
    // alive until the native payload is destroyed.
    #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
    memory: Option<Arc<crate::guest_heap::GuestHeap>>,
    // A detached shared handle pins the exact mapping independently of any
    // producer Store. Native payloads may retain backing-store pointers until
    // release, including after their producing worker has exited.
    #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
    shared_memory: Option<wasmer::SharedMemory>,
}

impl std::fmt::Debug for MessageCharge {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("MessageCharge")
            .field("bytes", &self.bytes)
            .finish_non_exhaustive()
    }
}

impl MessageCharge {
    pub(crate) fn reserve(budget: Arc<ResourceBudget>, bytes: u64) -> Result<Self, OverBudget> {
        budget.try_charge(Pool::SerializedMessage, bytes)?;
        Ok(Self {
            budget,
            bytes,
            #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
            memory: None,
            #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
            shared_memory: None,
        })
    }

    #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
    pub(crate) fn bind_memory(
        &mut self,
        memory: Arc<crate::guest_heap::GuestHeap>,
        shared_memory: Option<wasmer::SharedMemory>,
    ) {
        self.memory = Some(memory);
        self.shared_memory = shared_memory;
    }

    #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
    fn belongs_to_memory(&self, memory: &Arc<crate::guest_heap::GuestHeap>) -> bool {
        self.memory
            .as_ref()
            .is_some_and(|bound| Arc::ptr_eq(bound, memory))
    }

    pub(crate) fn shrink(&mut self, retained_bytes: u64) -> bool {
        if retained_bytes > self.bytes {
            return false;
        }
        self.budget
            .uncharge(Pool::SerializedMessage, self.bytes - retained_bytes);
        self.bytes = retained_bytes;
        true
    }
}

impl Drop for MessageCharge {
    fn drop(&mut self) {
        self.budget.uncharge(Pool::SerializedMessage, self.bytes);
    }
}

#[derive(Debug)]
pub(crate) struct PendingMessages {
    handles: Mutex<HashMap<u32, MessageEntry>>,
    closed: AtomicBool,
}

#[derive(Debug)]
struct MessageEntry {
    charge: Arc<MessageCharge>,
    legacy: bool,
}

impl PendingMessages {
    pub(crate) fn new() -> Arc<Self> {
        Arc::new(Self {
            handles: Mutex::new(HashMap::new()),
            closed: AtomicBool::new(false),
        })
    }

    /// Takes ownership after the bridge has created a message. The caller
    /// drops the bridge handle itself if insertion fails.
    pub(crate) fn insert(&self, id: u32, charge: MessageCharge) -> Result<(), MessageCharge> {
        self.insert_kind(id, charge, false)
    }

    pub(crate) fn insert_legacy(
        &self,
        id: u32,
        charge: MessageCharge,
    ) -> Result<(), MessageCharge> {
        self.insert_kind(id, charge, true)
    }

    fn insert_kind(
        &self,
        id: u32,
        charge: MessageCharge,
        legacy: bool,
    ) -> Result<(), MessageCharge> {
        if id == 0 {
            return Err(charge);
        }
        let mut handles = self.handles.lock().expect("poisoned message registry");
        if self.closed.load(Ordering::Acquire) {
            return Err(charge);
        }
        if handles.contains_key(&id) || handles.try_reserve(1).is_err() {
            return Err(charge);
        }
        handles.insert(
            id,
            MessageEntry {
                charge: Arc::new(charge),
                legacy,
            },
        );
        Ok(())
    }

    /// An atomic ownership check and removal. The charge remains live in the
    /// returned guard until the native take/drop operation has completed.
    pub(crate) fn take(&self, id: u32) -> Option<Arc<MessageCharge>> {
        self.take_kind(id, false)
    }

    /// Deserialization may only access native backing stores while an env
    /// attached to the producer's exact memory is alive. Leave a mismatched
    /// payload queued so the producer can still release it.
    #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
    pub(crate) fn take_for_memory(
        &self,
        id: u32,
        memory: &Arc<crate::guest_heap::GuestHeap>,
    ) -> Option<Arc<MessageCharge>> {
        let mut handles = self.handles.lock().expect("poisoned message registry");
        let entry = handles.get(&id)?;
        if entry.legacy || !entry.charge.belongs_to_memory(memory) {
            return None;
        }
        handles.remove(&id).map(|entry| entry.charge)
    }

    pub(crate) fn take_legacy(&self, id: u32) -> Option<Arc<MessageCharge>> {
        self.take_kind(id, true)
    }

    fn take_kind(&self, id: u32, legacy: bool) -> Option<Arc<MessageCharge>> {
        let mut handles = self.handles.lock().expect("poisoned message registry");
        if handles.get(&id)?.legacy != legacy {
            return None;
        }
        handles.remove(&id).map(|entry| entry.charge)
    }

    /// Legacy deserialization borrows a payload; only explicit release
    /// consumes it. The native bridge holds a shared lease across the read,
    /// so a concurrent release cannot free the bytes underneath V8.
    #[cfg(not(all(target_arch = "wasm32", feature = "js")))]
    pub(crate) fn lease_legacy_for_memory(
        &self,
        id: u32,
        memory: &Arc<crate::guest_heap::GuestHeap>,
    ) -> Option<Arc<MessageCharge>> {
        self.handles
            .lock()
            .expect("poisoned message registry")
            .get(&id)
            .and_then(|entry| {
                (entry.legacy && entry.charge.belongs_to_memory(memory))
                    .then(|| Arc::clone(&entry.charge))
            })
    }

    /// Discard queued payloads once the instance has stopped and guest work
    /// has drained. Cloned Wasmer hooks may otherwise retain the context.
    pub(crate) fn close_and_clear(&self) {
        self.closed.store(true, Ordering::Release);
        self.clear();
    }

    fn clear(&self) {
        let handles = {
            let mut guard = self.handles.lock().expect("poisoned message registry");
            std::mem::take(&mut *guard)
        };
        for id in handles.keys().copied() {
            unsafe { snapi_bridge_unofficial_message_drop(id) };
        }
    }
}

impl Drop for PendingMessages {
    fn drop(&mut self) {
        self.clear();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pending_messages_are_context_owned_and_charged() {
        let budget = ResourceBudget::with_memory_limit(100);
        let first = PendingMessages::new();
        let second = PendingMessages::new();
        let mut charge = MessageCharge::reserve(Arc::clone(&budget), 80).unwrap();
        assert!(charge.shrink(20));
        assert_eq!(budget.snapshot().serialized_message, 20);
        first.insert(1, charge).unwrap();
        assert!(second.take(1).is_none());
        drop(first.take(1));
        assert_eq!(budget.snapshot().serialized_message, 0);
    }
}
