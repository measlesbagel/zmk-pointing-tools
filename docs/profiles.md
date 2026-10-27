# Tuning profiles and configuration handoff

Each tuning target and parameter has a stable machine-readable identity. The
local tuner uses those identities instead of session-local numeric IDs when
exporting and importing profiles.

## Profile format

Profiles are versioned JSON documents:

```json
{
  "schema": "zmk-pointing-tools/tuning-profile",
  "version": 1,
  "exportedAt": "2026-01-01T00:00:00.000Z",
  "targets": [
    {
      "stableId": "right-scroll-batcher",
      "kind": 4,
      "label": "right-scroll-batcher",
      "devicetreePath": "/zpt_right_scroll_batcher",
      "parameters": [
        {
          "key": "steps-per-meter",
          "devicetreeProperty": "steps-per-meter",
          "value": 4000,
          "compiled": 3445
        }
      ]
    }
  ]
}
```

`stableId` and each parameter `key` determine import matching. Labels,
devicetree paths, property names, and compiled values are included for review
and configuration handoff, but are not used to silently substitute an unknown
target or parameter.

An import is fully validated against the connected firmware before any request
is sent. Unknown targets, unknown parameters, duplicate entries, wrong target
kinds, and out-of-range values reject the whole profile on the host. Values for
each target are then sent as one atomic firmware transaction. Different targets
are separate transactions.

Imports are temporary previews. They never write flash.

A calibration export (see [`calibration.md`](calibration.md)) is a profile
whose targets hold only the recommended values, with the evidence behind them
in an extra `calibration` object that import ignores.

## Git-tracked defaults

**Copy config** creates a devicetree overlay with every parameter of every
target, using the firmware-advertised node paths and property names:

```dts
/* right-scroll-batcher (right-scroll-batcher) */
&{/zpt_right_scroll_batcher} {
    steps-per-meter = <4000>;
    report-interval-ms = <16>;
};

/* right-constraint (right-constraint) */
&{/zpt_right_constraint} {
    /delete-property/ discard-unclassified;
    idle-timeout-ms = <120>;
    fold-interval-ms = <0>;
};
```

Boolean values use a property when enabled and `/delete-property/` when
disabled. Parameters that share a devicetree property are one array, written
in parameter-id order, so an acceleration curve comes out as:

```dts
/* cursor-acceleration (cursor-acceleration) */
&{/zpt_cursor_acceleration} {
    curve-speeds-mm-per-s = <0 8 20 45 90 160 260 400>;
    curve-sensitivity-percent = <65 66 73 99 159 225 264 283>;
    speed-rise-ms = <10>;
    speed-fall-ms = <5>;
};
```

The tuner draws such a curve as a chart with draggable points, preset shapes
(S-curve, Natural, Power, Classic, Synchronous, Flat) that sample into the
eight points, and a gain view. Edits stay in the page until **Preview curve**
sends all sixteen values as one batch. **Simulate** offers the smoke keymap's
acceleration and coalescer targets, so the editor works without a keyboard. Review the generated overlay and either incorporate its values into
the original nodes or commit the overlay in the consuming ZMK configuration.
The generated text is a handoff aid, not an automatic repository edit.

A stage's `stable-id` is its target's stable id, so keep it unique and durable.
Changing it intentionally prevents older profiles from matching the stage.
