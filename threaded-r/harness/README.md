# Harness

This directory contains the operational harness for agent-friendly iteration.

## Goals

- provide one discoverable command per validation stage,
- enforce strict-vs-drop-in runtime modes,
- keep benchmark and check outputs in stable artifact locations,
- make package ABI compatibility checks routine.

## Directory layout

- `scripts/`: stage runners (`smoke`, `partial`, `full`, `release`, benchmark wrappers).
- `checks/`: R checks used by stage runners.
- `abi/`: package ABI/load compatibility checks.

## Modes

- `strict`: disallow unintentional leakage from system libraries/packages.
- `dropin`: allow normal user/system package paths for realistic compatibility checks.
- `TR_ALLOW_FRAMEWORK_SITE=1` (default): in strict mode, allow only the version/arch-matching macOS framework library path when this build intentionally appends it.

## Canonical commands

From `threaded-r/`:

```sh
make init-layout
make bootstrap
make docs-check
make doc-garden
make smoke
make partial
make full
make release
make bench-quick
make bench-full
make abi
```

`make bootstrap` provisions the local source/build baseline:

- clones/syncs upstream `r-source/`,
- creates pinned `r-source-ref/` worktree,
- configures and builds `builds/mtl` + `builds/ref`.

`make docs-check` is a mechanical knowledge-base guardrail. It verifies required
agent-facing docs exist, `AGENTS.md` stays concise, and plan docs include
status + decision-log sections.
