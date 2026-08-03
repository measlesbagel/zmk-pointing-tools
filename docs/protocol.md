# Host protocol

The first transport is USB CDC-ACM for use through Web Serial. The protocol is
otherwise transport-independent. All multi-byte integers are little-endian.

## Framing

```text
0x5a 0x50  type:u8  payload-length:u16  payload
```

The host initiates requests. Trace samples are unsolicited only while the host
has explicitly enabled telemetry.

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
| `0x81` | device → host | Describe response | described below |
| `0x82` | device → host | Acknowledgement | `enabled:u8, dropped:u32` |
| `0x83` | device → host | Tuning targets | described below |
| `0x84` | device → host | Tuning target description | described below |
| `0x85` | device → host | Tuning result | described below |
| `0x86` | device → host | Parameter help | described below |
| `0x90` | device → host | Trace sample | described below |

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

Protocol version 2 adds discoverable temporary tuning. Targets are runtime
processor instances rather than hard-coded left/right devices.

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
disconnects, but reset explicitly or on keyboard reboot. No command in protocol
versions 2 or 3 writes settings to flash.

### Parameter help

Protocol version 3 adds on-demand human-readable help without changing the
version 2 target-description format:

```text
target-id:u8
parameter-id:u8
description-length:u16
description:utf8[description-length]
```

Fetching help separately keeps target discovery compact and lets generic host
interfaces explain processor-defined settings without embedding a matching
catalog of parameter IDs.
