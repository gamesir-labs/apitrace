# bundle-finalize

`bundle-finalize` is the offline finalization tool for apitrace trace bundles.
It is part of apitrace's own tool distribution, not a DXMT integration detail.

The capture process writes the raw event stream under `raw/`. This tool first
materializes that capture into the final bundle representation, then performs
the expensive publish-time work:

- merge `assets.json` with `analysis/sideband-assets.json`
- hash asset files outside the game process
- deduplicate identical assets into content-addressed paths
- rewrite JSON/JSONL asset path references
- compile the authoritative D3D12 JSONL into `analysis/d3d12-dispatch.bin`, preserving one record
  per original event while compiling every payload to bounded typed nodes and preselecting the
  native handler opcode
- emit dispatch records directly while materializing fresh RAW events; if later normalization
  changes callstream semantics, rebuild them inside the already-required final object audit scan
  instead of scanning `callstream.jsonl` again only for dispatch
- remove duplicate asset files by default, while preserving any alias path that
  is still referenced after rewriting
- regenerate root `assets.json`
- regenerate `checksums.json`

The default native D3D12 retrace depends on that compiled dispatch artifact and streams it one
record at a time. Finalization therefore owns all reusable parsing, validation, and dispatch-route
selection work; retrace must not reparse the multi-GB JSONL, scan function names to select a
handler, or preload every event. A stale or missing artifact is an
explicit finalization error rather than a reason to fall back to a slower replay interpreter.

Finalize performance is part of the format contract. Dispatch compilation uses newline-aligned
chunks and `--jobs N`, publishes chunks in source order, and reports
`compiled_dispatch_records`, `compiled_dispatch_bytes`, and `compiled_dispatch_ms`. The old D3D12
replay model is not generated or retained by a normal finalize; use `--persist-replay-model-only`
only for offline validation and differential analysis.

RAW materialization writes `callstream.jsonl` through a sibling temporary file and atomically
publishes it only after the complete committed prefix has been decoded. A failed or interrupted
run must leave the previous readable callstream intact. When the committed RAW prefix is older
than a materialized callstream whose recorded checksum sizes still match, repeated finalize runs
reuse that output instead of decoding every RAW event again. If the compiled dispatch is missing or
has an old format version, the dispatch-only fast path rebuilds it without reloading the full asset
index or rerunning the other publication passes; if it is current, finalize only verifies its header
and source generation. The summary reports `raw_to_final_reused=true` and
`finalize_fast_path=dispatch_rebuilt|verified`.

Build it from apitrace:

```sh
cmake -S . -B build-finalize -G Ninja \
  -DAPITRACE_BUILD_BUNDLE_FINALIZE=ON
cmake --build build-finalize --target apitrace_bundle_finalize
```

Usage:

```sh
bundle-finalize [--dry-run] [--keep-duplicates] [--jobs N] [--no-progress] <trace-bundle>
```

Raw capture materialization is the only supported normal finalization path and
is enabled unconditionally; there is no format-selection option. Because that
first stage creates `callstream.jsonl` and provisional assets, `--dry-run`
cannot be used for the initial materialization. Run a normal finalization first
and use `bundle-check` for a non-mutating integrity audit.

An already materialized bundle without `raw/commit.meta` can still be reopened
for offline maintenance (for example reference repair or integrity tests). This
does not provide a second capture path: all new captures are written through
the RAW stream and are materialized automatically whenever RAW data is present.

When stderr is an interactive TTY, `bundle-finalize` prints stage progress to
stderr by default. The final `bundle-finalize:` summary remains on stdout for
scripts. Use `--no-progress` to suppress interactive progress, or `--progress`
to force it when stderr is not detected as a TTY.

By default, resumed finalization trusts existing content-addressed canonical
asset files when their size matches the source asset hash that was just
computed. This avoids re-hashing duplicate canonical files on large bundles.
Use `--verify-existing-canonical` to restore strict re-hashing of existing
canonical files before reuse; use `bundle-check --verify-hashes` for a complete
post-finalize corruption audit.

By default, JSONL rewrite passes parse only records that may need asset-path or
blob-id updates after the tail-truncation pass has ensured a complete final
record. Use `--verify-jsonl-records` when you want `bundle-finalize` to parse
every JSONL record during reference rewriting as an additional integrity check.
