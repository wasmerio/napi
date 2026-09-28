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

#[derive(Debug)]
pub(crate) struct MessageCharge {
    budget: Arc<ResourceBudget>,
    bytes: u64,
}

impl MessageCharge {
    pub(crate) fn reserve(budget: Arc<ResourceBudget>, bytes: u64) -> Result<Self, OverBudget> {
        budget.try_charge(Pool::SerializedMessage, bytes)?;
        Ok(Self { budget, bytes })
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
    pub(crate) fn lease_legacy(&self, id: u32) -> Option<Arc<MessageCharge>> {
        self.handles
            .lock()
            .expect("poisoned message registry")
            .get(&id)
            .and_then(|entry| entry.legacy.then(|| Arc::clone(&entry.charge)))
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
