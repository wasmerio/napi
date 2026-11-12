# Guest state import

`napi_extension_wasmer_v0.unofficial_napi_register_guest_state` accepts three
Wasm32 integers for the environment ID, memory offset, and size. A native V8
guest can use the import to read common values and callback arguments directly
from its own linear memory. The host-JS backend rejects registration and the
guest continues using the ordinary imports.

Register a four-byte-aligned block in memory with a fixed base. The size
selects the layout. All integers in the block are little-endian.

| Size | Offset | Data |
| --- | --- | --- |
| 16 | 0–15 | Four 32-bit handles for undefined, null, true, and false |
| 20 | 16 | One byte indicating whether an exception is pending |
| 80 | 20–39 | Five 32-bit fields for callback info, argument count, receiver, new target, and data |
| 80 | 40–71 | The first eight argument handles, with undefined past the actual count |
| 80 | 72–79 | A 64-bit current realm datum |

Larger layouts include the preceding fields. The constant handles remain
valid for the environment's lifetime and do not belong to a handle scope.
The provider updates the callback frame before calling the guest. Read the
frame only on the environment's execution thread, and only when its callback
info matches the current callback. The provider clears the marker when a
callback returns, including on return from a nested callback. The receiver,
new target, and argument handles belong to the callback's scope. A zero
new target means the callback is not a constructor call.

A successful registration replaces the previous block. Pass size zero to
withdraw it, with any offset. Keep the block allocated and writable until
withdrawal, replacement, or environment release succeeds. The provider also
withdraws automatically when the store is dropped, even when environment
release is refused. Reads from the block do not update the environment's
last-error record. Use the corresponding imports when that side effect is
needed or the callback frame is unavailable.
