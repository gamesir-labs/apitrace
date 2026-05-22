# AGENTS.md

This file is the repository-specific guide for work inside `apitrace/`.

## Project State

- This repository is in the architecture-design stage.
- Prioritize boundary definition, directory layout, class and struct skeletons,
  naming, and TODO placement over feature-complete implementations.
- When a task is ambiguous, prefer preserving flexibility and separable module
  boundaries rather than filling in behavior early.

## Build System

- Use CMake only.
- Do not reintroduce Meson files, Meson scripts, or dual-build-system guidance.
- Primary local commands:
  - `cmake -S . -B build/cmake -DCMAKE_BUILD_TYPE=Debug`
  - `cmake --build build/cmake`
  - `git diff --check -- . ':(exclude)build'`

## Documentation

- Development docs live under `docs/`.
- Keep development and architecture docs in Chinese for reviewability.
- Before changing storage or replay architecture, read:
  - `docs/OVERVIEW.md`
  - `docs/ARCHITECTURE.md`
  - `docs/TRACE_LAYOUT.md`
  - `docs/CAPTURE.md`
  - `docs/RETRACE.md`

## Trace Storage Direction

- The target physical format is a trace bundle directory, not a single archive
  file and not `trace.1` / `trace.2` style sharding.
- Root bundle entry files are:
  - `checksums.json`
  - `callstream.jsonl`
- Shader, texture, buffer, and similar assets must be stored as separate files
  under type-specific directories such as `shaders/`, `textures/`, and
  `buffers/`.
- Non-asset files should be semantically readable whenever practical.
- Raw assets should remain raw:
  - `DXBC`
  - `DXIL`
  - serialized root signatures
  - raw texture or buffer payloads

## Code Structure

- Use `src/` as the code root.
- Organize code by module first, then by file role inside each module.
- Public headers should live under module-local paths such as:
  - `src/api/include/`
  - `src/trace/include/`
  - `src/capture/include/`
  - `src/retrace/include/`
  - `src/d3d11/include/`
  - `src/d3d12/include/`
  - `src/metal/include/`
- Implementations should live beside them in module-local `src/` directories.
- Command-line entrypoints should live under `src/tools/<tool-name>/src/`.
- Do not reintroduce a top-level `include/` plus top-level `src/` split that
  primarily groups files by type instead of by module boundary.
- When introducing a new storage or runtime concept, prefer a small interface
  type in a module-local `include/` directory and a stub implementation in the
  matching module-local `src/` directory with explicit TODOs.

## Replay and CLI Constraints

- `retrace` is the user-facing replay CLI.
- Keep the CLI narrow: one bundle in, replay it.
- Shared replay logic belongs in library code, not duplicated between
  executables.
- Wine-hosted replay should still conceptually be `wine retrace.exe`, but the
  replay core must remain platform-neutral.

## Examples

- Architecture examples live under `examples/`.
- The sample trace bundle is for structure review, not for runtime correctness.
- Keep example files aligned with `docs/TRACE_LAYOUT.md`.

## Editing Rules

- Use `rg` / `rg --files` for discovery.
- Use `apply_patch` for manual edits.
- Keep changes scoped to the current architectural concern.
- Do not mutate `build/` artifacts as part of source edits.
- When implementation details are not settled, leave a precise English TODO at
  the boundary instead of guessing.

## Commit Format

Use the following conventional commit format:

```text
type(scope): concise imperative summary
```

Write commit messages in English.

The `type` is one of these keywords:

```text
feat, fix, docs, style, refactor, perf, test, build, ci, chore, revert, merge
```

Recommended `scope` values in this repository include:

```text
api, bundle, callstream, checksums, capture, retrace, d3d11, d3d12, dxbc, dxil, rootsig, shader, texture, buffer, pipeline, objects, metal, tools, docs, build, repo
```

Commit splitting guidance:

- Keep architecture docs, code skeletons, storage-layout changes, and tool
  entrypoint changes in separate commits when they are logically distinct.
- Prefer narrow scopes tied to the owning module or concern.
- Do not mix unrelated runtime behavior changes with documentation-only edits
  unless the user explicitly asks for a single combined commit.

## Verification

- For architecture-only changes, prefer:
  - `cmake -S . -B build/cmake -DCMAKE_BUILD_TYPE=Debug`
  - `cmake --build build/cmake`
  - `git diff --check -- . ':(exclude)build'`
- State clearly if a task only updated skeletons and docs rather than runtime
  behavior.
