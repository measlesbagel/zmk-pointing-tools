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
      "stableId": "left-scroll",
      "kind": 1,
      "label": "Left scroll",
      "devicetreePath": "/zpt_left_scroll",
      "parameters": [
        {
          "key": "scale-multiplier",
          "devicetreeProperty": "scale-multiplier",
          "value": 7,
          "compiled": 7
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

## Git-tracked defaults

**Copy config** creates a devicetree overlay using the firmware-advertised node
paths and property names:

```dts
/* Left scroll (left-scroll) */
&{/zpt_left_scroll} {
    scale-multiplier = <7>;
    /delete-property/ discard-unclassified;
};
```

Boolean values use a property when enabled and `/delete-property/` when
disabled. Review the generated overlay and either incorporate its values into
the original nodes or commit the overlay in the consuming ZMK configuration.
The generated text is a handoff aid, not an automatic repository edit.

Set `tuning-id` to a unique, durable value on every tunable target. Changing
that ID intentionally prevents older profiles from matching the target.
