# Host protocol

The first transport is USB CDC-ACM for use through Web Serial. The protocol is
otherwise transport-independent. All multi-byte integers are little-endian.

## Framing

```text
0x5a 0x50  type:u8  payload-length:u16  payload
```

The host initiates requests. Trace samples are unsolicited only while the host
has explicitly enabled telemetry.

## Messages

| Type | Direction | Name | Payload |
| --- | --- | --- | --- |
| `0x01` | host → device | Describe request | empty |
| `0x02` | host → device | Telemetry control | `enabled:u8` |
| `0x03` | host → device | Heartbeat | empty |
| `0x81` | device → host | Describe response | described below |
| `0x82` | device → host | Acknowledgement | `enabled:u8, dropped:u32` |
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

This initial protocol is deliberately read-only except for enabling telemetry.
Capability discovery, temporary previews, resetting to compiled values, and
optional persistence will be added without making persistence the default.
