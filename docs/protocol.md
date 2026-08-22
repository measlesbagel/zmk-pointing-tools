# Host protocol

The first transport is USB CDC-ACM for use through Web Serial. The protocol is
otherwise transport-independent. All multi-byte integers are little-endian.

The current protocol is version 7. Firmware and host are updated together; the
tuner rejects every other version instead of carrying compatibility parsers or
feature gates for older firmware.

## Framing

```text
0x5a 0x50  type:u8  payload-length:u16  payload
```

The host initiates requests. Stage-state samples are unsolicited only while
the host has explicitly enabled the corresponding per-target level.

Hosts send at most one tuning request at a time and wait for its matching
response before advancing discovery. This provides transport-level flow
control for firmware with small serial receive buffers; bulk parameter-help
requests must not be emitted as an unbounded burst.

## Messages

| Type | Direction | Name | Payload |
| --- | --- | --- | --- |
| `0x01` | host → device | Describe request | empty |

| `0x03` | host → device | Heartbeat | empty |
| `0x04` | host → device | List tuning targets | empty |
| `0x05` | host → device | Describe tuning target | `target-id:u8` |
| `0x06` | host → device | Preview tuning value | `target-id:u8, parameter-id:u8, value:i32` |
| `0x07` | host → device | Reset tuning target | `target-id:u8`; `0xff` resets all |
| `0x08` | host → device | Get parameter help | `target-id:u8, parameter-id:u8` |
| `0x09` | host → device | Get target metadata | `target-id:u8` |
| `0x0a` | host → device | Get parameter metadata | `target-id:u8, parameter-id:u8` |
| `0x0b` | host → device | Preview parameter batch | described below |
| `0x0c` | host → device | Processor-state control/status | empty to query, or `target-id:u8, level:u8` |
| `0x0d` | host → device | List pointing devices | empty |
| `0x0e` | host → device | Describe pointing device | `device-id:u8` |
| `0x0f` | host → device | Preview device CPI | `device-id:u8, cpi:u16` |
| `0x81` | device → host | Describe response | described below |
| `0x82` | device → host | Acknowledgement | `state-dropped:u32` |
| `0x83` | device → host | Tuning targets | described below |
| `0x84` | device → host | Tuning target description | described below |
| `0x85` | device → host | Tuning result | described below |
| `0x86` | device → host | Parameter help | described below |
| `0x87` | device → host | Target metadata | described below |
| `0x88` | device → host | Parameter metadata | described below |
| `0x89` | device → host | Processor-state status | described below |
| `0x8a` | device → host | Pointing devices | described below |
| `0x8b` | device → host | Pointing device description | described below |
| `0x91` | device → host | Stage-state sample | described below |

### Describe response

```text
protocol-version:u8
```

The describe response is the protocol version handshake; the tuner rejects
every other version. While the host keeps streaming alive, it sends a
heartbeat at least every five seconds. Firmware disables all state levels
automatically when the host disappears.

## Runtime tuning

Targets are runtime instances rather than hard-coded left/right devices.
After the monolithic processors were removed, the registry reports zero
tunable targets; the protocol remains in place for stage-parameter
registration (tracked by #15).

### Tuning targets

```text
target-count:u8
repeat target-count times:
  target-id:u8
  target-kind:u8
  label-length:u8
  label:utf8[label-length]
```

Target kinds are `1` scroll, `2` text navigation, `3` noise filter, and `4`
pipeline stage. Kind `4` currently appears only on stage-state samples; no
stage parameters are tunable yet.

### Tuning target description

```text
target-id:u8
parameter-count:u8
repeat parameter-count times:
  parameter-id:u8
  value-type:u8
  minimum:i32
  maximum:i32
  step:i32
  compiled-value:i32
  current-value:i32
  label-length:u8
  unit-length:u8
  label:utf8[label-length]
  unit:utf8[unit-length]
```

Value type `0` is an integer and type `1` is a boolean represented by `0` or
`1`. Firmware validates ranges and parameter relationships before applying a
preview.

### Tuning result

```text
request-type:u8
status:u8
target-id:u8
parameter-id:u8
value:i32
```

Status values are success (`0`), unknown target (`1`), unknown parameter (`2`),
invalid value (`3`), and internal error (`4`). A successful preview returns the
effective current value.

Runtime values live only in RAM. They remain active when the web page
disconnects, but reset explicitly or on keyboard reboot. No protocol command
writes settings to flash.

### Parameter help

Human-readable parameter help is fetched on demand:

```text
target-id:u8
parameter-id:u8
description-length:u16
description:utf8[description-length]
```

Fetching help separately keeps target discovery compact and lets generic host
interfaces explain processor-defined settings without embedding a matching
catalog of parameter IDs.

### Stable profile metadata

Stable identities support profile export/import while retaining numeric IDs
for compact session requests. Target metadata is:

```text
target-id:u8
stable-id-length:u8
devicetree-path-length:u16
stable-id:utf8[stable-id-length]
devicetree-path:utf8[devicetree-path-length]
```

Parameter metadata is:

```text
target-id:u8
parameter-id:u8
key-length:u8
devicetree-property-length:u8
key:utf8[key-length]
devicetree-property:utf8[devicetree-property-length]
```

Stable target IDs are keyboard policy supplied by `tuning-id`. Parameter keys
are owned by each processor family. Devicetree paths and property names allow a
host to generate a reviewable overlay without assuming node labels.

### Atomic parameter preview

Related values can be updated as one target-level transaction:

```text
target-id:u8
value-count:u8
repeat value-count times:
  parameter-id:u8
  value:i32
```

The response is the ordinary tuning result with request type `0x0b` and the
number of applied values in its value field. Firmware validates every range,
duplicate, and processor-level relationship before replacing current settings;
if any value fails, none of that target's values change. A request contains at
most 20 values.

## Pointing devices

Physical pointing devices are described independently of tuning targets.
Entries come from the devicetree device table (`measlesbagel,zpt-pointing-device`);
numeric ids are dense session ids assigned in devicetree order, while stable
identities join the two halves of a split keyboard.

### List pointing devices

```text
device-count:u8
repeat device-count times:
  device-id:u8
  location:u8          /* 0 central-local, otherwise peripheral index */
  flags:u8             /* bit0 local-connected, bit1 settable */
  label-length:u8
  label:utf8[label-length]
```

The label is the entry's stable id. Entries owned by other halves are listed
without the local-connected flag until split-routed control reaches them.

### Describe pointing device

```text
stable-id-length:u8, stable-id:utf8[..]
devicetree-path-length:u16, devicetree-path:utf8[..]
current-cpi:u16
default-cpi:u16       /* compiled devicetree value */
settable:u8           /* 1 when capabilities follow */
if settable and discrete:
  value-count:u8, then value-count × cpi:u16   /* ascending */
if settable and not discrete:
  reserved:u8 (zero), min:u16, max:u16, step:u16
```

Unknown ids fail with an ordinary tuning result carrying request type `0x0e`
and status unknown target.

### Preview device CPI

The tuning result message carries the outcome with request type `0x0f`, the
device id in its parameter-id byte, and the effective value on success:
firmware validates the request against the declared capabilities and snaps to
the nearest supported value when needed, so a host compares requested against
effective to offer corrections. Previews are RAM-only and expire on reboot,
like tuning previews. Read-only devices reject previews with status invalid
value.

## Stage-state telemetry

Optional diagnostics are keyed by target id in the shared tuning/telemetry
target table. Tuning-registered targets take the first ids; pipeline stage
observers (`measlesbagel,zpt-pipeline-telemetry`) allocate the remaining ids at
init, and the status response enumerates every allocated target.

Control levels are off (`0`), decisions (`1`), and every frame (`2`). Target ID
`0xff` applies a level to every target. An empty request queries status without
changing levels. Every request receives:

```text
state-schema-version:u8
dropped-state-records:u32
shared-telemetry-queue-capacity:u16
target-count:u8
repeat target-count times:
  target-id:u8
  level:u8
  label-length:u8
  label:bytes[label-length]
```

Status schema version 2 added the per-target label so hosts can render
per-stage controls without tuning discovery. Tuning-registered targets report
their tuning label and pipeline stage targets report their stable id; labels
are capped at 27 bytes, the length of the longest current stable id.

State samples use schema version 1:

```text
target-id:u8
target-kind:u8
event:u8
intent:u8
flags:u16
timestamp-ms:u32
sequence:u32
values:i32[10]
```

The sequence counter orders samples within the bounded shared queue; the state
drop counter in status and heartbeat acknowledgements makes saturation
visible.

Pipeline stage observers emit one sample per stage decision: suppression,
unclassified discard, gate qualification, axis-intent changes, report flushes,
and text actions. `event` is frame (`1`) for decisions and flush (`2`) for
report flushes. `values[0]` carries the stage event kind and `values[1]` the
event quantity (the new intent, the emitted step magnitude, or the action id);
the remaining values are reserved zeros. `intent` is the new intent for
intent-change events and zero otherwise. Stage samples use these flag bits:

| Bit | Meaning |
| --- | --- |
| 0 | axis intent changed |
| 1 | frame suppressed by the physical-keypress guard |
| 2 | unclassified motion discarded |
| 3 | semantic output emitted |
| 4 | pending movement qualified as intentional |

The decisions level emits stage decisions only; the every-frame level
is reserved for future per-frame stage producers and currently adds nothing.
Reserved values and unknown flags must be ignored for forward compatibility.
Firmware disables all state levels after the ordinary host heartbeat timeout
or an explicit all-targets-off request.
