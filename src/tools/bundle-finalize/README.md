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
- remove duplicate asset files by default, while preserving any alias path that
  is still referenced after rewriting
- regenerate root `assets.json`
- regenerate `checksums.json`

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
