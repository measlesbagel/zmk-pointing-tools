<!-- DRAFT: design sketch for issue #13. Not implemented; wire formats and
     names are proposals for review, not contract. -->

# Pointing device discovery and split-safe CPI control

Per-issue sketch for #13: let a generic tuner discover physical pointing
devices, show what the hardware supports, and change sensor resolution on the
half that owns the sensor — without persisting anything and without assuming
continuous CPI steps or a specific driver.

## Topology: beyond two halves

Nothing above assumes exactly two units. The device table's location byte
addresses any half (0 local, 1..n peripherals), tunnel requests carry the
target half explicitly, and response correlation is by sequence number, so
it does not matter which chain returns a frame.

A dongle-style setup (USB-powered central such as a Prospector running the
pipeline and telemetry, both keyboard halves as wireless peripherals) works
unchanged: sensors stream frames to the central exactly as today, all
processing stays central-side, and the tuner talks USB CDC to the dongle.

Wiring requirements for three or more units:

- One `zmk,input-split` node per peripheral half, each with a distinct
  configured `reg` number - ZMK's peripheral identity is this configured
  reg (stable across boots), and each node carries its own processor chain,
  so every half gets its own source-ingress instance feeding the central
  router.
- Attach the `zpt-sensor-control-proxy` processor to every
  `zmk,input-split` chain that owns controllable devices. Multi-attach is
  safe: proxy state is a singleton, and sequence correlation prevents
  cross-talk between chains.
- Align the device table's location byte with the half's `zmk,input-split`
  reg (location = reg + 1). For two-half splits there is exactly one
  peripheral and the mapping is trivially correct; verify the alignment on
  the first three-plus-unit build before relying on remote control.

## Goals and non-goals

Goals:

- Describe physical pointing devices independently from pipelines, stages, and
  tuning targets.
- Report current, compiled/default, and supported CPI values per device.
- Preview (RAM-only) CPI per device from the web tuner, including devices on a
  split peripheral.
- Handle disconnected peripherals and drivers without runtime control cleanly.
- Keep the device abstraction driver-neutral (PAW3222 first, not last).

Non-goals:

- No persistence to flash (#24 owns lifecycle).
- No calibration/gain recommendation (#23 builds on this).
- No changes to motion processing semantics; CPI only feeds the existing
  source normalization boundary.

## What already exists

| Piece | Where | Relevance |
| --- | --- | --- |
| Updatable per-source CPI with pending-motion rejection | `zpt_motion_source_state` (`src/source/motion_source.c`) | The ingestion-side hook is built; control lands here on the central |
| One-way split motion transport with sequence evidence | compact codec (`docs/compact-split-codec.md`) | Peripheral→central only; proves the input-event tunnel pattern |
| Central→peripheral command channel | ZMK split transport `INVOKE_BEHAVIOR` (`zmk/split/transport/types.h`) | Existing, typed, reaches arbitrary peripheral behaviors with two u32 params |
| Host protocol v6: framing, one-outstanding-request flow control, stable-id metadata, shared target table | `docs/protocol.md`, `src/service/telemetry.c` | Discovery extends it; does not rework it |

The missing pieces are a firmware device model, a small driver-facing control
API, host protocol messages, and the reverse-direction split tunnel.

## Design overview

```text
web tuner
   │  USB CDC (protocol v7 additions)
   ▼
telemetry service (central)
   │  device table lookup
   ├────────────► local sensor control ──► local sensor driver
   │
   └── central→peripheral: INVOKE_BEHAVIOR(zpt-sensor-control)
          opcode + device index + payload
       peripheral→central: control frame in a reserved input event
          request-seq + status + payload, decoded by a proxy source
```

Three independent layers:

1. **Device table** — devicetree-declared pointing devices with stable ids,
   owned half, and capability data. Compiled into both halves so ids agree
   without runtime negotiation.
2. **Sensor control API** — a tiny optional driver facet (`get` / `set` /
   supported values). Devices whose driver lacks the facet are discoverable
   and read-only, reporting the devicetree value as both current and default.
3. **Transport** — local calls go straight to the driver; peripheral devices
   route through a request/response tunnel over ZMK's existing split channels.

## 1. Device table

One devicetree node per physical pointing device, independent of pipelines:

```text
zpt_devices {
    compatible = "measlesbagel,zpt-device-table";

    left_trackball: left-trackball {
        compatible = "measlesbagel,zpt-pointing-device";
        stable-id = "left-trackball";
        sensor = <&trackball>;            /* driver device phandle */
        location = <ZPT_DEVICE_PERIPHERAL>; /* local | peripheral */
        resolution-cpi = <800>;
        /* Discrete supported values from the datasheet, ascending. */
        cpi-values = <200 400 800 1200 1600>;
    };
};
```

Rules:

- `stable-id` is the cross-half join key and the host-facing identity;
  numeric `device-id`s are session-local allocations, like tuning target ids.
- Both halves compile the same table; each instantiates entries matching its
  own `location`. A peripheral agent answers only for its own entries, so a
  central request names the device by stable id and the routing layer resolves
  which half executes it.
- Supported values come from devicetree, not from probing at boot: authored
  from the datasheet, reviewable in overlays, and validated by the driver when
  one is present. Continuous-range devices may instead declare
  `cpi-min`/`cpi-max`/`cpi-step`; exactly one representation is allowed.
- The table registers every device into the shared telemetry target table
  (new target kind `5`, "pointing device") so state-telemetry diagnostics can
  address devices with the same machinery as stages.
- Multiplicity is supported everywhere by construction (the table is a list;
  discovery responses are lists), but the reference hardware has exactly one
  trackball per half, so multi-local-device configurations are validated by
  fake devices in tests only. Host UI renders the list generically at no
  extra cost rather than hard-coding a single entry.

## 2. Sensor control API

A driver facet, not a base-class obligation:

```c
struct zpt_sensor_control_api {
    int (*get_cpi)(const struct device *dev, uint16_t *cpi);
    int (*set_cpi)(const struct device *dev, uint16_t cpi);      /* RAM only */
    int (*supported_count)(const struct device *dev, size_t *count);
    int (*supported_get)(const struct device *dev, size_t index, uint16_t *cpi);
};

/* Lookup returns NULL when the backing driver has no facet. */
const struct zpt_sensor_control_api *zpt_sensor_control(const struct device *sensor);
```

Semantics:

- `set_cpi` writes volatile registers only; reboot restores the devicetree
  value. It must never touch NVM (sensor-internal or ZMK settings).
- A device without the facet is still fully describable: current and default
  both report the devicetree `resolution-cpi`, supported list comes from DT,
  and preview attempts fail with `-ENOSYS`.
- The PAW3222 adapter is the first implementation; its register map decides
  whether unsupported requested values snap to the nearest step or reject.

On the central, an accepted value flows into the matching
`zpt_motion_source_state` via the existing inter-frame update (rejected while
frames are pending), so counts are never labelled with the wrong resolution.

## 3. Protocol additions (v6 → v7)

Framing, heartbeat, flow control ("one outstanding request"), and status codes
are unchanged. The version bumps to 7 because describe gains fields; the tuner
already rejects mismatched versions outright, so there is no compatibility
burden.

New requests reuse the tuning result response (`0x85`) where possible, keeping
host error handling uniform.

### List pointing devices — `0x0d`

```text
device-count:u8
repeat device-count times:
  device-id:u8
  location:u8              /* 0 local, 1..n peripheral index */
  connected:u8             /* 0 unknown, 1 connected, 2 disconnected */
  caps:u8                  /* bit0 settable, bit1 discrete list, bit2 range */
  label-length:u8
  label:utf8[label-length]
```

`connected` reflects live split connection status for peripherals and is
always `1` for local devices. No round-trip to the peripheral is needed to
list it.

### Describe pointing device — `0x0e`, payload `device-id:u8`

```text
stable-id-length:u8     stable-id:utf8[..]
dt-path-length:u16      dt-path:utf8[..]
current-cpi:u16
default-cpi:u16         /* compiled devicetree value */
caps:u8                 /* as above */
repeat per caps:
  discrete: count:u8, then count × cpi:u16   /* bit1 */
  range:    min:u16, max:u16, step:u16        /* bit2 */
```

Describe may block briefly on a peripheral read; see timeouts below. The
response always includes DT-derived data even when the peripheral is
unreachable, with `current-cpi = default-cpi` and caps cleared of the
settable bit plus a `not-connected` status if the caller asked to query live.

### Preview CPI — `0x0f`, payload `device-id:u8, cpi:u16`

Response: tuning result (`0x85`) with request type `0x0f`. Statuses reuse the
tuning codes; invalid-but-closest-known values return the nearest supported
value in the result's value field so UIs can offer a one-tap correction.

Preview is RAM-only and expires on reboot, identical to tuning previews. No
new persistence path is introduced.

## 4. Split tunnel

Central → peripheral uses the existing typed command channel rather than a new
ZMK message type (no upstream fork):

- The peripheral declares a `zpt,sensor-control` behavior. Its binding cells
  are zero; everything travels in the invocation parameters:
  - `param1`: `device-local-index:u8 | opcode:u8<<8 | seq:u16<<16`
  - `param2`: payload (`cpi:u16` for SET, reserved otherwise)
- Opcodes: `GET_CAPS`, `GET_CPI`, `SET_CPI`.

Peripheral → central responses ride back as ordinary split input events on a
reserved event code — the same tunnel trick the compact encoder uses for
motion. The default reservation is `INPUT_REL_MISC` (0x09): no driver in
Zephyr or ZMK emits it, boards may override via devicetree if a future sensor
disagrees, and acceptance requires a magic byte plus a live request sequence
number, so a colliding producer would be counted as garbage frames rather
than misread. Response frames:

```text
magic:u8  seq:u16  status:u8  payload-len:u8  payload[≤6]
```

decoded by a central-side `zpt-input-processor-sensor-control-proxy`. A single
input event carries one response frame, so payloads stay small by construction
(CPI values, capability counts, one 16-bit value).

Operational rules:

- Exactly one outstanding control request system-wide, mirroring the host
  protocol's flow control; the telemetry service serializes anyway.
- Request correlation by `seq`; stale or unsolicited frames are dropped and
  counted (surfaced through the existing dropped-state counter style).
- Timeout 500 ms per attempt, no automatic retry of SET (it is idempotent but
  the UI should confirm state via GET). While a peripheral is disconnected,
  requests fail immediately with `not-connected` from split transport status
  instead of waiting for the timeout.
- If the peripheral reboots mid-session, its volatile CPI resets; the next
  GET reports the devicetree value and the tuner refreshes.

## 5. Testing plan

- **ztest (`tests/unit`)** — device-table registration and stable-id joining,
  protocol parser paths for `0x0d–0x0f` including resync-after-garbage,
  proxy decode with stale/duplicate/dropped frames, timeout and
  not-connected paths against mock transports (the `vnd,serial` +
  fake-behavior patterns from #85 carry over). Multi-local-device tables and
  UI listing paths are covered with fake devices, since the physical
  hardware is one trackball per half.
- **Host tests** — the pure parts: supported-value validation/snapping and
  caps encoding compile natively behind the module's host build like other
  core logic.
- **Trace replay** — unaffected; CPI changes occur between frames by rule.
  A replay directive for mid-trace CPI switches is deliberately out of scope
  until #23 needs it.

## 6. Implementation slices

Each slice is one stack layer, shippable and testable alone:

1. Device table bindings + registry + ztest (no protocol, no behavior).
2. Sensor-control API + PAW3222 adapter + local-device path + ztest.
3. Protocol v7 messages (list/describe/preview) for **local** devices +
   docs/protocol.md update.
4. Split tunnel: peripheral behavior, proxy input processor, seq/timeout
   semantics + ztest; enable peripheral devices in the protocol. Before
   relying on it, bench-validate command round-trip while streaming (see
   resolved decisions) — if latency disappoints, control still works but the
   docs must set expectations instead of the 500 ms budget.
5. Web tuner: device panel, read-only first, then preview controls.

## Resolved decisions

1. **ZMK dependency.** The design consumes two existing ZMK surfaces: the
   split transport's central command (`INVOKE_BEHAVIOR`) and split input
   event forwarding. Both exist in the pinned revision
   (`fa33e35f11d2b15311973cda9fb89dcd2376888c`; verified in
   `zmk/app/include/zmk/split/transport/types.h`), so there is no upstream
   work required to start. What cannot be answered from source is timing:
   central→peripheral commands share BLE connection time with streaming
   input events, so round-trip latency during heavy motion is an empirical
   question. Slice 4 therefore includes a bench check — stream maximum-rate
   motion from the peripheral while issuing repeated GET/SET previews and
   confirm round-trips fit the 500 ms timeout budget. The failure mode is
   soft either way: sequence numbers, timeouts, and immediate
   disconnected-status mean slow transport degrades to slower UI feedback,
   never to wrong values (SET is idempotent and confirmed by GET).
2. **Tunnel event code:** `INPUT_REL_MISC` (0x09) — defined in Zephyr's
   input-event bindings, emitted by no driver in either tree, overridable per
   board in devicetree, and guarded by magic byte plus live sequence number
   so even a collision surfaces as dropped-garbage counts, not corruption.
3. **Describe of unreachable peripheral:** degrade, do not error — return
   DT-derived identity, capabilities minus the settable bit, and
   `current-cpi = default-cpi`; live reads report `not-connected`.
4. **Device multiplicity:** firmware and protocol stay generic (lists end to
   end); host UI renders the list without assuming one entry. Physical
   validation is single-trackball-per-half, so multi-device cases are
   exercised with fake devices in ztest and host tests only.
