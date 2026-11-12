//! Exercise realm snapshots through the actual Wasmer imports and V8 bridge.
#![cfg(not(target_arch = "wasm32"))]

use std::path::PathBuf;
use std::process::Command;
use std::time::{SystemTime, UNIX_EPOCH};

use wasmer::{AsStoreMut, Instance, Memory, Module, Store};
use wasmer_napi::{NapiCtx, SnapshotEnvironment};

const OK: i32 = 0;
const INVALID_ARG: i32 = 1;
const GENERIC_FAILURE: i32 = 9;
const FRESH: u32 = 1;
const TEMPLATE: u32 = 2;
const RESTORE: u32 = 3;
const CLASS: u32 = 1;
const METHOD: u32 = 2;

const REALM: u32 = 16;
const GLOBAL: u32 = 20;
const ROOT: u32 = 24;
const SCOPE: u32 = 32;
const FUNCTION: u32 = 40;
const PROTOTYPE: u32 = 44;
const INDEX: u32 = 48;
const LENGTH: u32 = 52;
const KEY: &[u8] = b"wasmer-guest-realm-binding-test";
const PAYLOAD: &[u8] = b"payload";

macro_rules! call {
    ($guest:expr, $name:literal $(, $arg:expr)* $(,)?) => {{
        let arguments = [$(wasmer::Value::from($arg)),*];
        let function = $guest.instance.exports.get_function($name).unwrap().clone();
        let result = function.call(&mut $guest.store, &arguments).expect($name);
        result[0].unwrap_i32()
    }};
}

struct Guest {
    store: Store,
    instance: Instance,
    memory: Memory,
    env: u32,
    owner: u32,
}

impl Guest {
    fn new(ctx: NapiCtx, callbacks: bool) -> Self {
        let mut store = Store::default();
        let wasm = wat::parse_str(include_str!("wat_guest/realm_bindings.wat")).unwrap();
        let module = Module::new(&store, wasm).unwrap();
        let hooks = ctx.runtime_hooks();
        let (imports, state) = hooks
            .additional_imports(&module, &mut store.as_store_mut())
            .unwrap();
        let instance = Instance::new(&mut store, &module, &imports).unwrap();
        let memory = instance.exports.get_memory("memory").unwrap().clone();
        hooks
            .configure_instance(
                &module,
                &mut store.as_store_mut(),
                &instance,
                Some(&memory),
                state,
            )
            .unwrap();
        let mut guest = Self {
            store,
            instance,
            memory,
            env: 0,
            owner: 0,
        };
        if !callbacks {
            guest
                .instance
                .exports
                .get_typed_function::<(), ()>(&guest.store, "remove_callback")
                .unwrap()
                .call(&mut guest.store)
                .unwrap();
        }
        assert_eq!(call!(guest, "create"), OK);
        guest.env = guest.word(8);
        guest.owner = guest.word(12);
        guest
    }

    fn put(&self, ptr: u32, bytes: &[u8]) {
        self.memory
            .view(&self.store)
            .write(ptr.into(), bytes)
            .unwrap();
    }

    fn get<const N: usize>(&self, ptr: u32) -> [u8; N] {
        let mut bytes = [0; N];
        self.memory
            .view(&self.store)
            .read(ptr.into(), &mut bytes)
            .unwrap();
        bytes
    }

    fn memory_end(&self) -> u32 {
        u32::try_from(self.memory.view(&self.store).data_size()).unwrap()
    }

    fn word(&self, ptr: u32) -> u32 {
        u32::from_le_bytes(self.get(ptr))
    }

    fn put_word(&self, ptr: u32, value: u32) {
        self.put(ptr, &value.to_le_bytes());
    }

    fn realm(&mut self, mode: u32, index: u32, owner: u32) -> i32 {
        call!(
            self, "realm", self.env, 0u32, mode, index, owner, REALM, GLOBAL, ROOT
        )
    }

    fn binding(&mut self, kind: u32, parent: u32, length: u32, prototype: u32) -> i32 {
        let name = if kind == CLASS { 176u32 } else { 171u32 };
        call!(
            self, "binding", self.env, kind, name, 4u32, 1u32, 0u32, length, parent, 0u32, 0u32,
            0u32, FUNCTION, prototype
        )
    }

    fn enter(&mut self, realm: u32) -> u64 {
        assert_eq!(call!(self, "enter", self.env, realm, SCOPE), OK);
        u64::from_le_bytes(self.get(SCOPE))
    }

    fn set(&mut self, object: u32, name: u32, value: u32) {
        assert_eq!(call!(self, "set", self.env, object, name, value), OK);
    }

    fn install(&mut self, realm: u32, global: u32) -> u32 {
        let scope = self.enter(realm);
        assert_eq!(call!(self, "object", self.env, ROOT), OK);
        let root = self.word(ROOT);
        self.set(global, 160, root);
        // napi_property_descriptor: the getter and setter share one bridge
        // registration, so an accessor must select its side by role.
        for (index, value) in [165, 0, 0, 1, 2, 0, 4, 0].into_iter().enumerate() {
            self.put_word(224 + 4 * index as u32, value);
        }
        assert_eq!(call!(self, "properties", self.env, root, 1u32, 224u32), OK);
        assert_eq!(self.binding(METHOD, 0, 3, 0), OK);
        self.set(root, 171, self.word(FUNCTION));
        assert_eq!(self.binding(CLASS, 0, 2, PROTOTYPE), OK);
        self.set(root, 176, self.word(FUNCTION));
        assert_ne!(self.word(PROTOTYPE), 0);
        assert_eq!(call!(self, "leave", self.env, scope), OK);
        root
    }

    fn verify_bindings(&mut self, realm: u32, owner: u32) {
        let signed = owner as i32;
        let source = format!(
            "(() => {{ const d = Object.getOwnPropertyDescriptor(root, 'value');              d.set.call(root);              return Number(d.get.call(root, 1) === {signed} && root.meth() === {signed} &&                root.meth.length === 3 && !Object.hasOwn(root.meth, 'prototype') &&                root.Ctor.length === 2 && new root.Ctor() instanceof root.Ctor); }})()"
        );
        self.put(1024, source.as_bytes());
        assert_eq!(
            call!(self, "eval", self.env, realm, source.len() as u32),
            OK
        );
        assert_eq!(call!(self, "result", self.env), 1);
        assert_eq!(call!(self, "setter_calls"), 1);
    }

    fn release(&mut self) {
        assert_eq!(call!(self, "release", self.owner, 0u32), OK);
    }
}

#[test]
fn fresh_realms_validate_and_dispatch_native_bindings() {
    let mut guest = Guest::new(NapiCtx::default(), true);
    guest.put_word(REALM, 99);
    assert_eq!(guest.realm(FRESH, 1, 0), INVALID_ARG);
    assert_eq!(guest.word(REALM), 99);
    assert_eq!(
        call!(
            guest, "realm", guest.env, 0u32, FRESH, 0u32, 0u32, REALM, REALM, ROOT
        ),
        INVALID_ARG
    );
    let short_owner_output = guest.memory_end() - 4;
    assert_eq!(
        call!(guest, "owner", guest.env, short_owner_output),
        INVALID_ARG
    );

    assert_eq!(call!(guest, "open_scope", guest.env, SCOPE), OK);
    let handle_scope = guest.word(SCOPE);
    assert_eq!(guest.realm(FRESH, 0, 0), OK);
    let stale = guest.word(REALM);
    assert_eq!(call!(guest, "close_scope", guest.env, handle_scope), OK);
    assert_eq!(call!(guest, "enter", guest.env, stale, SCOPE), INVALID_ARG);

    let owner = 0x8000_0043;
    assert_eq!(guest.realm(FRESH, 0, owner), OK);
    let realm = guest.word(REALM);
    assert_ne!(realm, 0);
    assert_eq!(realm, guest.word(GLOBAL));
    assert_eq!(guest.word(ROOT), 0);
    assert_eq!(guest.binding(METHOD, realm, 0, 0), INVALID_ARG);
    assert_eq!(guest.binding(METHOD, 0, 256, 0), INVALID_ARG);
    guest.install(realm, realm);
    guest.verify_bindings(realm, owner);
    guest.release();
}

#[test]
fn realm_and_binding_outputs_fail_cleanly_when_the_value_table_is_full() {
    let ctx = NapiCtx::builder().total_memory_bytes(64 << 20).build();
    let limit = ctx.budget().value_handle_limit().unwrap() as u32;
    let mut guest = Guest::new(ctx, true);
    assert_eq!(call!(guest, "open_scope", guest.env, SCOPE), OK);
    let handle_scope = guest.word(SCOPE);
    assert_eq!(
        call!(guest, "fill_values", guest.env, limit - 1),
        (limit - 1) as i32
    );

    guest.put_word(FUNCTION, 71);
    guest.put_word(PROTOTYPE, 72);
    assert_eq!(guest.binding(CLASS, 0, 0, PROTOTYPE), GENERIC_FAILURE);
    assert_eq!((guest.word(FUNCTION), guest.word(PROTOTYPE)), (71, 72));
    assert_eq!(guest.binding(METHOD, 0, 0, 0), OK);
    assert_ne!(guest.word(FUNCTION), 0);
    guest.put_word(REALM, 73);
    assert_eq!(guest.realm(FRESH, 0, 0), GENERIC_FAILURE);
    assert_eq!(guest.word(REALM), 73);
    assert_eq!(call!(guest, "close_scope", guest.env, handle_scope), OK);
    assert_eq!(guest.realm(FRESH, 0, 0), OK);
    assert_ne!(guest.word(REALM), 0);
    guest.release();
}

struct Stage(PathBuf);

impl Drop for Stage {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

fn verify_restore(mut guest: Guest, callbacks: bool) {
    assert_eq!(
        call!(guest, "snapshot_data", guest.env, 0u32, 0u32, LENGTH),
        OK
    );
    if callbacks {
        assert_eq!(guest.word(LENGTH), PAYLOAD.len() as u32);
        assert_eq!(
            call!(guest, "snapshot_data", guest.env, 248u32, 3u32, LENGTH),
            OK
        );
        assert_eq!(guest.get::<3>(248), PAYLOAD[..3]);
        assert_eq!(guest.word(LENGTH), PAYLOAD.len() as u32);
        let owner = 0x8000_0083;
        assert_eq!(guest.realm(RESTORE, 0, owner), OK);
        assert_ne!(guest.word(ROOT), 0);
        let realm = guest.word(REALM);
        guest.verify_bindings(realm, owner);
    } else {
        assert_eq!(guest.word(LENGTH), 0);
        assert_eq!(guest.realm(RESTORE, 0, 0), GENERIC_FAILURE);
        assert_eq!(guest.realm(FRESH, 0, 0), OK);
    }
    guest.release();
}

#[test]
fn snapshots_bake_and_restore_guest_callbacks_across_processes() {
    const PATH: &str = "WASMER_NAPI_TEST_REALM_ARTIFACT";
    if let Some(path) = std::env::var_os(PATH) {
        let bytes = std::fs::read(path).unwrap();
        // SAFETY: the parent test baked these bytes with the same V8 build in
        // a private staging directory. A wrong key is rejected before use.
        assert!(unsafe { SnapshotEnvironment::restore(&bytes, b"wrong key") }.is_err());
        let snapshot = unsafe { SnapshotEnvironment::restore(&bytes, KEY) }.unwrap();
        drop(bytes);
        // Both environments retain the same validated artifact. Rejecting the
        // manifest in the env without the callback table must stay local.
        std::thread::scope(|threads| {
            let (ready, both_live) = std::sync::mpsc::sync_channel(0);
            let clone = snapshot.clone();
            threads.spawn(move || {
                let guest =
                    Guest::new(NapiCtx::default().with_snapshot_environment(clone), true);
                ready.send(()).unwrap();
                verify_restore(guest, true);
            });
            let guest = Guest::new(
                NapiCtx::default().with_snapshot_environment(snapshot),
                false,
            );
            both_live.recv().unwrap();
            verify_restore(guest, false);
        });
        return;
    }
    let nonce = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let stage = Stage(
        std::env::temp_dir().join(format!("napi-guest-realm-{}-{nonce}", std::process::id())),
    );
    std::fs::create_dir(&stage.0).unwrap();
    let path = stage.0.join("staged.snapshot");
    let builder = SnapshotEnvironment::builder(KEY, &path).unwrap();
    let mut guest = Guest::new(NapiCtx::default().with_snapshot_environment(builder), true);
    assert_eq!(guest.realm(TEMPLATE, 0, 29), OK);
    let realm = guest.word(REALM);
    let root = guest.install(realm, realm);

    // Reject the output slot before sealing or otherwise mutating the builder.
    let short_index_output = guest.memory_end() - 2;
    assert_eq!(
        call!(
            guest,
            "commit",
            guest.env,
            realm,
            root,
            190u32,
            PAYLOAD.len() as u32,
            short_index_output
        ),
        INVALID_ARG
    );
    assert_eq!(
        call!(
            guest,
            "commit",
            guest.env,
            realm,
            root,
            190u32,
            PAYLOAD.len() as u32,
            INDEX
        ),
        OK
    );
    assert_eq!(guest.word(INDEX), 0);
    assert_eq!(call!(guest, "object", guest.env, ROOT), INVALID_ARG);
    assert_eq!(guest.realm(FRESH, 0, 0), INVALID_ARG);
    guest.release();
    assert!(path.is_file());

    let child = Command::new(std::env::current_exe().unwrap())
        .args([
            "--exact",
            "snapshots_bake_and_restore_guest_callbacks_across_processes",
        ])
        .env(PATH, &path)
        .output()
        .unwrap();
    assert!(
        child.status.success(),
        "stdout:\n{}\nstderr:\n{}",
        String::from_utf8_lossy(&child.stdout),
        String::from_utf8_lossy(&child.stderr)
    );
}
