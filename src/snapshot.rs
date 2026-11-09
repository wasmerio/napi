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
const SNAPSHOT_ROLE_RESTORE: u32 = 2;

#[repr(C)]
pub(crate) struct SnapshotEnvOptions {
    size: u32,
    version: u32,
    role: u32,
    reserved: u32,
    artifact: *mut c_void,
    key: *const c_void,
    key_length: usize,
    output_path: *const c_char,
    output_path_length: usize,
}

unsafe extern "C" {
    fn unofficial_napi_open_snapshot(
        bytes: *const c_void,
        length: usize,
        key: *const c_void,
        key_length: usize,
        artifact_out: *mut *mut c_void,
    ) -> i32;
    fn unofficial_napi_close_snapshot(artifact: *mut c_void);
}

struct SnapshotHandle(NonNull<c_void>);

impl SnapshotHandle {
    fn as_ptr(&self) -> *mut c_void {
        self.0.as_ptr()
    }
}

// The native handle holds an immutable artifact and only lends it to calls
// that retain their own reference. The Arc keeps the handle live until all
// such calls return, so only its final owner closes it.
unsafe impl Send for SnapshotHandle {}
unsafe impl Sync for SnapshotHandle {}

impl Drop for SnapshotHandle {
    fn drop(&mut self) {
        // SAFETY: we own the handle returned by open_snapshot.
        unsafe { unofficial_napi_close_snapshot(self.as_ptr()) }
    }
}

/// An immutable, validated native snapshot artifact. Clones share one handle.
#[derive(Clone)]
pub struct SnapshotArtifact(Arc<SnapshotHandle>);

impl std::fmt::Debug for SnapshotArtifact {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("SnapshotArtifact").finish_non_exhaustive()
    }
}

impl SnapshotArtifact {
    /// Validate the artifact and key and copy the bytes into provider-owned memory.
    ///
    /// # Safety
    /// `bytes` must be a trusted artifact produced by the compatible provider.
    /// The format's integrity checks do not authenticate V8 heap data.
    pub unsafe fn open(bytes: &[u8], key: &[u8]) -> Result<Self, i32> {
        let mut artifact = ptr::null_mut();
        // SAFETY: the caller vouches for the source. The slices remain live
        // throughout the native call, which copies both.
        let status = unsafe {
            unofficial_napi_open_snapshot(
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
        NonNull::new(artifact)
            .map(|artifact| Self(Arc::new(SnapshotHandle(artifact))))
            .ok_or(NAPI_GENERIC_FAILURE)
    }
}

#[derive(Debug)]
struct BuildRequest {
    key: Box<[u8]>,
    output_path: Box<[u8]>,
}

#[derive(Clone)]
#[doc(hidden)]
pub struct SnapshotBuilder(Arc<Mutex<Option<BuildRequest>>>);

/// Selects the snapshot role of environments belonging to a [`crate::NapiCtx`].
#[derive(Clone)]
pub enum SnapshotEnvironment {
    /// The first successful creation using this request becomes a builder.
    /// Clones share the request. Later creations are fresh.
    Builder(SnapshotBuilder),
    /// Every environment configured with this value restores from this artifact.
    Restore(SnapshotArtifact),
}

impl std::fmt::Debug for SnapshotEnvironment {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(match self {
            Self::Builder(_) => "SnapshotEnvironment::Builder",
            Self::Restore(_) => "SnapshotEnvironment::Restore",
        })
    }
}

impl SnapshotEnvironment {
    /// Create a one-shot builder request for a private staging path.
    ///
    /// Use a distinct path for each concurrent builder. To retry a build after
    /// the builder environment has been created, create a new request.
    pub fn builder(key: &[u8], output_path: &Path) -> Result<Self, i32> {
        let output_path = output_path.as_os_str().as_encoded_bytes();
        if key.is_empty()
            || u32::try_from(key.len()).is_err()
            || output_path.is_empty()
            || output_path.contains(&0)
        {
            return Err(NAPI_INVALID_ARG);
        }
        Ok(Self::Builder(SnapshotBuilder(Arc::new(Mutex::new(Some(
            BuildRequest {
                key: key.into(),
                output_path: output_path.into(),
            },
        ))))))
    }

    pub(crate) fn create_env(&self, create: impl FnOnce(*const SnapshotEnvOptions) -> i32) -> i32 {
        let mut options = SnapshotEnvOptions {
            size: std::mem::size_of::<SnapshotEnvOptions>() as u32,
            version: SNAPSHOT_ENV_OPTIONS_VERSION,
            role: 0,
            reserved: 0,
            artifact: ptr::null_mut(),
            key: ptr::null(),
            key_length: 0,
            output_path: ptr::null(),
            output_path_length: 0,
        };
        match self {
            Self::Builder(builder) => {
                let mut pending = builder.0.lock().expect("poisoned snapshot builder");
                let Some(request) = pending.as_ref() else {
                    drop(pending);
                    return create(ptr::null());
                };
                options.role = SNAPSHOT_ROLE_BUILDER;
                options.key = request.key.as_ptr().cast();
                options.key_length = request.key.len();
                options.output_path = request.output_path.as_ptr().cast();
                options.output_path_length = request.output_path.len();
                let status = create(&options);
                if status == 0 {
                    *pending = None;
                }
                status
            }
            Self::Restore(artifact) => {
                options.role = SNAPSHOT_ROLE_RESTORE;
                options.artifact = artifact.0.as_ptr();
                create(&options)
            }
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
            request.create_env(|options| {
                assert!(!options.is_null());
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
                            request.create_env(|options| {
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
