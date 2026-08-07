# Host protocol

The first transport is USB CDC-ACM for use through Web Serial. The protocol is
otherwise transport-independent. All multi-byte integers are little-endian.

The current protocol is version 5. Firmware and host are updated together; the
tuner rejects every other version instead of carrying compatibility parsers or
feature gates for older firmware.

## Framing

```text
0x5a 0x50  type:u8  payload-length:u16  payload
```

The host initiates requests. Trace and processor-state samples are unsolicited
only while the host has explicitly enabled their respective streams.

Hosts send at most one tuning request at a time and wait for its matching
response before advancing discovery. This provides transport-level flow
control for firmware with small serial receive buffers; bulk parameter-help
requests must not be emitted as an unbounded burst.

## Messages

| Type | Direction | Name | Payload |
| --- | --- | --- | --- |
| `0x01` | host → device | Describe request | empty |
| `0x02` | host → device | Telemetry control | `enabled:u8` |
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
| `0x81` | device → host | Describe response | described below |
| `0x82` | device → host | Acknowledgement | `enabled:u8, trace-dropped:u32, state-dropped:u32` |
| `0x83` | device → host | Tuning targets | described below |
| `0x84` | device → host | Tuning target description | described below |
| `0x85` | device → host | Tuning result | described below |
| `0x86` | device → host | Parameter help | described below |
| `0x87` | device → host | Target metadata | described below |
| `0x88` | device → host | Parameter metadata | described below |
| `0x89` | device → host | Processor-state status | described below |
| `0x90` | device → host | Trace sample | described below |
| `0x91` | device → host | Processor-state sample | described below |

### Describe response

```text
protocol-version:u8
stream-count:u8
repeat stream-count times:
  pointing-device-id:u8
  stage:u8
  label-length:u8
  label:utf8[label-length]
```

Device IDs are keyboard-defined and need not mean left/right. Stage `0` is
conventionally raw input and stage `1` is processed output.

While telemetry is active, the host sends a heartbeat at least every five
seconds. Firmware disables streaming automatically when the host disappears.

### Trace sample

```text
pointing-device-id:u8
stage:u8
timestamp-ms:u32
sequence:u32
x:i32
y:i32
wheel:i32
horizontal-wheel:i32
```

## Runtime tuning

Targets are runtime processor instances rather than hard-coded left/right
devices.

### Tuning targets

```text
target-count:u8
repeat target-count times:
  target-id:u8
  target-kind:u8
  label-length:u8
  label:utf8[label-length]
```

Target kind `1` is a synchronized scroll processor and kind `2` is a
gesture-locked text-navigation processor.

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

## Semantic processor-state telemetry

Optional diagnostics are associated with the same runtime target IDs used for
tuning. State telemetry is observational, starts off, does not persist, and is
independent of raw/output trace control.

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
```

“Decisions” emits intent/lifecycle transitions, suppression transitions,
discard decisions, and semantic output. “Every frame” additionally emits each
complete processor input frame and no-output flush. The latter can approach
the sensor report rate and should only be enabled briefly. Trace and state
records share the advertised bounded queue; independent trace/state drop
counters make saturation visible in status and heartbeat acknowledgements.

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

The sequence counter is shared with trace samples, allowing hosts to order
state decisions alongside raw and output records even when timestamps match.
Intent values are undecided (`0`), free (`1`), horizontal (`2`), and vertical
(`3`). Event values are frame (`1`) and flush (`2`). Flag bits are:

| Bit | Meaning |
| --- | --- |
| 0 | gesture reset after first frame, idle timeout, or policy change |
| 1 | axis intent changed |
| 2 | frame suppressed by the physical-keypress guard |
| 3 | keypress-guard suppression entered or exited |
| 4 | unclassified motion discarded |
| 5 | semantic output emitted |
| 6 | horizontal output clipped to the HID range |
| 7 | vertical output clipped to the HID range |

The ten values are defined by target kind and event:

| Target/event | Values 0–9 |
| --- | --- |
| scroll frame | input X/Y, horizontal/vertical energy, undecided X/Y, pending X/Y, remainder X/Y |
| scroll flush | output H/V, horizontal/vertical energy, pre-flush undecided X/Y, post-flush pending X/Y, remainder X/Y |
| text frame | input X/Y, accumulated X/Y, direction, then five reserved zeros |

Text direction is none (`-1`), left (`0`), right (`1`), up (`2`), or down
(`3`). Reserved values and unknown flags must be ignored for forward
compatibility. Firmware disables all state levels after the ordinary host
heartbeat timeout or an explicit all-targets-off request.
