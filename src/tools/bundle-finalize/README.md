# bundle-finalize

`bundle-finalize` is the offline finalization tool for apitrace trace bundles.
It is part of apitrace's own tool distribution, not a DXMT integration detail.

The capture process should prioritize writing raw event streams and sideband
asset shards. This tool performs the expensive publish-time work afterwards:

- merge `assets.json` with `analysis/sideband-assets.json`
- hash asset files outside the game process
- deduplicate identical assets into content-addressed paths
- rewrite JSON/JSONL asset path references
- remove duplicate asset files by default
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
bundle-finalize [--dry-run] [--keep-duplicates] [--jobs N] <trace-bundle>
```

Run `--dry-run` first on large captures to inspect the expected rewrite and
removal counts before modifying the bundle.
