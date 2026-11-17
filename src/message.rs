//! Ownership and memory charges of the messages a context's environments
//! send each other (`unofficial_napi_message_*`, behind `postMessage`).
//!
//! The provider's message ids are process-wide, because the sender and the
//! receiver are different environments, possibly in different stores of the
//! context. A guest may only take or drop a message its own context created:
//! every id is checked against the context's [`PendingMessages`] first, so a
//! guest cannot consume another application's message by guessing its id.
//!
//! A message stays charged to the context's budget
//! ([`Pool::SerializedMessage`]) from its creation until it is taken, dropped,
//! or the context stops ([`crate::NapiRuntimeControl::terminate_all`]) or is
//! dropped, which also frees every message still queued.

use std::{
    collections::HashMap,
    sync::{Arc, Mutex},
};

use crate::budget::{Pool, ResourceBudget};
use crate::snapi::snapi_bridge_unofficial_message_drop as drop_provider_message;

/// A message's charge to [`Pool::SerializedMessage`], returned when dropped.
#[derive(Debug)]
pub(crate) struct MessageCharge {
    budget: Arc<ResourceBudget>,
    bytes: u64,
}

impl MessageCharge {
    /// Charges `bytes` softly: a refusal is reported to the sender as a clone
    /// error, and the application keeps running.
    pub(crate) fn try_new(budget: &Arc<ResourceBudget>, bytes: u64) -> Option<Self> {
        budget
            .try_charge_soft(Pool::SerializedMessage, bytes)
            .ok()?;
        Some(Self {
            budget: Arc::clone(budget),
            bytes,
        })
    }
}

impl Drop for MessageCharge {
    fn drop(&mut self) {
        self.budget.uncharge(Pool::SerializedMessage, self.bytes);
    }
}

/// The messages a context created that were not taken or dropped yet.
#[derive(Debug, Default)]
pub(crate) struct PendingMessages {
    /// `None` once the context stopped: no message is accepted anymore.
    messages: Mutex<Option<HashMap<u32, MessageCharge>>>,
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
    pub(crate) fn insert(&self, id: u32, charge: MessageCharge) -> bool {
        let mut messages = self.messages.lock().expect("poisoned message registry");
        match messages.as_mut() {
            Some(messages) if id != 0 && !messages.contains_key(&id) => {
                messages.insert(id, charge);
                true
            }
            _ => {
                drop(messages);
                // SAFETY: the provider just created `id` for this caller, and
                // no one else knows it.
                unsafe { drop_provider_message(id) };
                drop(charge);
                false
            }
        }
    }

    /// Removes one of this context's messages, for the caller to take or drop
    /// in the provider. `None` for an id this context does not own (or no
    /// longer owns). The message stays charged until the returned charge is
    /// dropped.
    pub(crate) fn remove(&self, id: u32) -> Option<MessageCharge> {
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
        for (id, charge) in messages.into_iter().flatten() {
            // SAFETY: the registry owned `id`, and removing it under the lock
            // gave it to this call alone.
            unsafe { drop_provider_message(id) };
            drop(charge);
        }
    }
}

impl Drop for PendingMessages {
    fn drop(&mut self) {
        self.close();
    }
}
