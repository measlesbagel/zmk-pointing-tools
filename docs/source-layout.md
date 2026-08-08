# Source layout

The firmware source and public headers follow the ownership boundaries in the
[composable motion pipeline architecture](architecture.md). Directories are
created when they have an implementation; the repository does not retain empty
placeholder directories.

| Directory | Responsibility |
| --- | --- |
| `core/` | Typed signal execution, pipeline lifecycle, and shared runtime machinery |
| `source/` | Source-independent frame reconstruction and source metadata handling |
| `stage/` | Ordered reusable transforms, gates, filters, estimators, constraints, mappers, quantizers, and batchers |
| `sink/` | Thin terminal output adapters for already-decided values |
| `platform/zmk/` | ZMK and Zephyr integration around reusable source, pipeline, stage, and sink code |
| `observer/` | Non-mutating trace and semantic-state observation contracts |
| `service/` | Cross-cutting runtime services such as discovery, tuning, and telemetry transport |
| `legacy_processor/` | Current monolithic processor models and ZMK adapters retained until composed replacements reach parity |

Public headers under `include/zmk/pointing_tools/` mirror these roles. Platform
integration without a public contract remains private under `src/platform/`.

## Stage strategies and policies

A strategy is an implementation of a particular stage contract, not a peer of
the stage itself. Strategies stay beneath their stage capability:

```text
stage/
└── smoothing/
    ├── one_euro.c
    └── heading_aware.c
```

A policy is one condition within a stage decision. It remains in that stage's
configuration or implementation unless it becomes substantial enough for a
nested helper:

```text
stage/
└── motion_gate/
    ├── coherent_displacement.c
    └── policy/
        └── keypress_guard.c
```

Top-level `strategy/` and `policy/` directories are intentionally avoided: they
would separate an algorithm from the contract and lifecycle that give it
meaning.

## Transitional processors

Files under `legacy_processor/` remain supported by the current Bridges
firmware. “Legacy” describes their pre-pipeline architecture, not an unused or
unmaintained status. Their host replay tests remain authoritative until each
capability is migrated and the old implementation can be deleted.

New reusable capabilities should not be added to a monolithic processor merely
for convenience. Generic behavior belongs in a source component or stage, with
the existing processor changed only when necessary to preserve current
firmware during migration.
