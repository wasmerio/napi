# NAPI source ownership

This directory is maintained as source in Wasmer-next so WASIX integration changes
can be reviewed and delivered with their runtime consumers.

The source was imported from `https://github.com/wasmerio/napi` at
`bdc247406e9cecd46888ce648a62d51ed2732378`, preserving the native GC/frame repair
and legacy runtime path migration. The protected native refill recovery is tracked against draft `1ac102cde91dec6710865f22037ac44fccc862fd`, relative to its parent
`d81f068`.

The nested QuickJS dependency remains at
`ff1471cf525483ea1e5b8030d5ecf274ed00eb70` and is registered in the repository's
root `.gitmodules`. Existing copyright notices and the package's MIT license
declaration are retained. No separate upstream LICENSE file was present in the
imported tree.

Local adaptations connect the binding to the WASIX v2 instantiation and protected
memory lease contracts. Historical fixtures remain with this source; the import
itself does not establish runtime parity or supported-profile acceptance.
