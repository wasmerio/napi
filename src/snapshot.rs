//! Host-owned, per-context configuration for native realm snapshots.

use std::{
    ffi::{c_char, c_void},
    path::Path,
    ptr::{self, NonNull},
    sync::{Arc, Mutex},
};

const NAPI_INVALID_ARG: i32 = 1;
const NAPI_GENERIC_FAILURE: i32 = 9;
const SNAPSHOT_ENV_OPTIONS_VERSION: u32 = 1;
const SNAPSHOT_ROLE_BUILDER: u32 = 1;
#[repr(C)]
pub(crate) struct SnapshotEnvOptions {
    size: u32,
    version: u32,
    role: u32,
    reserved: u32,
    artifact: *const c_void,
    artifact_length: usize,
    key: *const c_void,
    key_length: usize,
    output_path: *const c_char,
    output_path_length: usize,
}

#[repr(C)]
pub(crate) struct SnapshotHandle {
    _private: [u8; 0],
}

unsafe extern "C" {
    #[link_name = "NapiV8OpenSnapshot"]
    fn open_snapshot(
        bytes: *const c_void,
        length: usize,
        key: *const c_void,
        key_length: usize,
        artifact_out: *mut *mut SnapshotHandle,
    ) -> i32;
    #[link_name = "NapiV8CloseSnapshot"]
    fn close_snapshot(artifact: *mut SnapshotHandle);
}

struct RetainedSnapshot(NonNull<SnapshotHandle>);

impl RetainedSnapshot {
    fn as_ptr(&self) -> *mut SnapshotHandle {
        self.0.as_ptr()
    }
}

// The native handle holds an immutable artifact and only lends it to calls
// that retain their own reference. The Arc keeps the handle live until all
// such calls return, so only its final owner closes it.
unsafe impl Send for RetainedSnapshot {}
unsafe impl Sync for RetainedSnapshot {}

impl Drop for RetainedSnapshot {
    fn drop(&mut self) {
        // SAFETY: we own the handle returned by open_snapshot.
        unsafe { close_snapshot(self.as_ptr()) }
    }
}

#[derive(Debug)]
struct BuildRequest {
    key: Box<[u8]>,
    output_path: Box<[u8]>,
}

#[derive(Clone)]
enum SnapshotRequest {
    Builder(Arc<Mutex<Option<BuildRequest>>>),
    Restore(Arc<RetainedSnapshot>),
}

/// Selects the snapshot role of environments belonging to a [`crate::NapiCtx`].
#[derive(Clone)]
pub struct SnapshotEnvironment(SnapshotRequest);

impl std::fmt::Debug for SnapshotEnvironment {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(match &self.0 {
            SnapshotRequest::Builder(_) => "SnapshotEnvironment::Builder",
            SnapshotRequest::Restore(_) => "SnapshotEnvironment::Restore",
        })
    }
}

impl SnapshotEnvironment {
    /// Create a one-shot builder request for a private staging path.
    ///
    /// Supply a new path in a private staging directory. On Windows the path
    /// must be ASCII. To retry after the builder env was created, create a
    /// new request. Clones share one request and only the first successful
    /// environment creation uses it. Later creations are fresh.
    pub fn builder(key: &[u8], output_path: &Path) -> Result<Self, i32> {
        let output_path = output_path.as_os_str().as_encoded_bytes();
        if key.is_empty()
            || u32::try_from(key.len()).is_err()
            || output_path.is_empty()
            || output_path.contains(&0)
            || (cfg!(windows) && !output_path.is_ascii())
        {
            return Err(NAPI_INVALID_ARG);
        }
        let request = BuildRequest {
            key: key.into(),
            output_path: output_path.into(),
        };
        Ok(Self(SnapshotRequest::Builder(Arc::new(Mutex::new(Some(
            request,
        ))))))
    }

    /// Validate the snapshot and key before creating environments.
    ///
    /// The provider copies the artifact once. All environments made with
    /// this configuration or its clones share the validated data.
    ///
    /// # Safety
    /// `bytes` must be a trusted artifact produced by the compatible provider.
    /// The format's integrity checks do not authenticate V8 heap data.
    pub unsafe fn restore(bytes: &[u8], key: &[u8]) -> Result<Self, i32> {
        let mut artifact = ptr::null_mut();
        // SAFETY: the caller vouches for the source. Both slices remain live
        // throughout the call, and the returned handle owns its artifact.
        let status = unsafe {
            open_snapshot(
                bytes.as_ptr().cast(),
                bytes.len(),
                key.as_ptr().cast(),
                key.len(),
                &mut artifact,
            )
        };
        if status != 0 {
            return Err(status);
        }
        let artifact = NonNull::new(artifact).ok_or(NAPI_GENERIC_FAILURE)?;
        Ok(Self(SnapshotRequest::Restore(Arc::new(RetainedSnapshot(
            artifact,
        )))))
    }

    pub(crate) fn create_env(
        &self,
        create: impl FnOnce(*const SnapshotEnvOptions, *mut SnapshotHandle) -> i32,
    ) -> i32 {
        match &self.0 {
            SnapshotRequest::Builder(builder) => {
                let mut pending = builder.lock().expect("poisoned snapshot builder");
                let Some(request) = pending.as_ref() else {
                    drop(pending);
                    return create(ptr::null(), ptr::null_mut());
                };
                let options = SnapshotEnvOptions {
                    size: std::mem::size_of::<SnapshotEnvOptions>() as u32,
                    version: SNAPSHOT_ENV_OPTIONS_VERSION,
                    role: SNAPSHOT_ROLE_BUILDER,
                    reserved: 0,
                    artifact: ptr::null(),
                    artifact_length: 0,
                    key: request.key.as_ptr().cast(),
                    key_length: request.key.len(),
                    output_path: request.output_path.as_ptr().cast(),
                    output_path_length: request.output_path.len(),
                };
                let status = create(&options, ptr::null_mut());
                if status == 0 {
                    *pending = None;
                }
                status
            }
            SnapshotRequest::Restore(artifact) => create(ptr::null(), artifact.as_ptr()),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn builder_retries_creation_and_shares_its_claim_across_clones() {
        let request =
            SnapshotEnvironment::builder(b"application key", Path::new("staged")).unwrap();
        assert_eq!(
            request.create_env(|options, artifact| {
                assert!(!options.is_null());
                assert!(artifact.is_null());
                NAPI_GENERIC_FAILURE
            }),
            NAPI_GENERIC_FAILURE,
        );

        let configured_count = std::thread::scope(|threads| {
            let attempts: Vec<_> = (0..4)
                .map(|_| {
                    let request = request.clone();
                    threads.spawn(move || {
                        let mut configured = false;
                        assert_eq!(
                            request.create_env(|options, artifact| {
                                assert!(artifact.is_null());
                                configured = !options.is_null();
                                0
                            }),
                            0,
                        );
                        configured
                    })
                })
                .collect();
            attempts
                .into_iter()
                .filter_map(|attempt| attempt.join().unwrap().then_some(()))
                .count()
        });
        assert_eq!(configured_count, 1);
    }
}
