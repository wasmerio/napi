# NAPI source ownership

This is the standalone wasmerio/napi `edge-wasixv2` branch, based on Edge's
`edge-napi-webassembly-metered` at `af20edeae76192e3cdd49c5966c34cc81d0192d6`.
It ports the WASIX v2 hooks, split engine and protected-memory integration
from napi `next` at `c17595ac738853fa3935096bf3ec28718c62deda` onto Edge's features.

The reference `next` tree originally imported napi at
`bdc247406e9cecd46888ce648a62d51ed2732378`, including native GC/frame repairs;
its protected native refill recovery tracked draft `1ac102cde91dec6710865f22037ac44fccc862fd`.
This port preserves those ownership protocols and restores Edge's upstream
provider updates lost through the recursive merge bases.

QuickJS is v0.17 at `89fb17a82ba1d38f605e01558c3928aacc4ffd54`, registered in
this repository's `.gitmodules`. Existing copyright and MIT license declarations
are retained. See `WASIXV2.md` for supported provider decisions, review repairs
and executed validation. Historical fixtures alone do not establish parity.
