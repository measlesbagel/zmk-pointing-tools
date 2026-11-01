# Host protocol

The protocol runs over USB CDC-ACM serial (Web Serial) and, optionally, over a
Bluetooth GATT service (Web Bluetooth). Both carry the same framed byte stream;
see [Transports](#transports). All multi-byte integers are little-endian.

The current protocol is version 8. Version 8 added input capture and the
capture drop counter in acknowledgements. Firmware and host are updated together; the
tuner rejects every other version instead of carrying compatibility parsers or
feature gates for older firmware.

## Framing

```text
0x5a 0x50  type:u8  payload-length:u16  payload
```

The host initiates requests. Stage-state samples are unsolicited only while
the host has explicitly enabled the corresponding per-target level, and
capture batches only while the host runs a recording.

Hosts send at most one tuning request at a time and wait for its matching
response before advancing discovery. This provides transport-level flow
control for firmware with small serial receive buffers; bulk parameter-help
requests must not be emitted as an unbounded burst.

## Transports

Every transport carries the framed byte stream above unchanged, and firmware
decodes each link separately. A request is answered on the link it arrived
on, and state samples go to the link that sent the latest request. Heartbeats,
the host timeout, and state levels are shared, so a host on either link keeps
telemetry alive. Firmware never cuts a frame short while a link stays up:
host decoders, the tuner's included, can't resynchronise after a truncated
frame.

### USB serial

The `measlesbagel,zpt-telemetry-uart` chosen node names a USB CDC-ACM port.
`CONFIG_ZMK_POINTING_TOOLS_TELEMETRY_UART` is on whenever that chosen node
exists, so a keyboard without it builds without the USB serial transport.
Writes block until the port takes each byte, so no frame is dropped.

### Bluetooth GATT

A GATT service on the central's host connection, beside ZMK's HID service.
`CONFIG_ZMK_POINTING_TOOLS_TELEMETRY_BLE` is on by default wherever the
telemetry service is (a `measlesbagel,zpt-telemetry` node in the keymap) on a
Bluetooth keyboard or split central; a keyboard config needs nothing extra.
It doesn't apply to split peripherals, which build without it and without a
warning, so a config shared by both halves stays as it is. A peripheral
without USB serial builds without the telemetry service at all. Turn the
Bluetooth transport off with:

```ini
CONFIG_ZMK_POINTING_TOOLS_TELEMETRY_BLE=n
```

Builds with it off don't include the service. Idle cost is near zero: the
service isn't advertised, the worker sleeps until a host writes, and telemetry
stays off until a host enables it.

Bluetooth also works without USB serial: a keymap with the
`measlesbagel,zpt-telemetry` node but no `measlesbagel,zpt-telemetry-uart`
chosen node (and no CDC-ACM settings) serves the protocol over Bluetooth only,
on the central. Where neither transport can run, such as a split peripheral
without USB serial, the service is left out; the build fails only if the
service is forced on with both transports turned off.

| | UUID | Properties |
| --- | --- | --- |
| Service | `f336b450-531c-49cf-9d5d-846a5db2398b` | primary |
| Request characteristic | `f336b451-531c-49cf-9d5d-846a5db2398b` | write, write without response |
| Response characteristic | `f336b452-531c-49cf-9d5d-846a5db2398b` | notify |

Browsers only expose services a page names, so a Web Bluetooth host lists the
service UUID in `optionalServices` (Chrome blocklists HID, which the keyboard
also exposes).

- **Security.** Both writing requests and subscribing need an encrypted link,
  and firmware rejects requests from a peer that isn't bonded with Insufficient
  Authentication, since requests change settings. The computer that uses the
  keyboard is already bonded, so this costs nothing there.
- **Requests.** The host writes request bytes to the request characteristic in
  chunks of at most 20 bytes, which fit the minimum ATT MTU. Firmware doesn't
  support long (prepared) writes, and Web Bluetooth doesn't expose the
  negotiated MTU. Chunks can split a frame anywhere. Use write with response
  (`writeValueWithResponse`): when firmware's receive buffer is full it
  rejects the chunk with an error the host sees, while a write without
  response is lost silently and corrupts the frame. When a different
  connection, or the same host after reconnecting, writes its first chunk,
  firmware discards any frame the previous connection left half-written.
  Subscribing to the response characteristic does the same, so a new tuner
  session on an unchanged connection also starts with a clean parser.
- **Responses and telemetry.** The host subscribes to notifications on the
  response characteristic before its first request. Firmware sends only to the
  connection that wrote the latest request. Each frame starts a new
  notification and spans as many as it needs, each up to the ATT MTU minus 3
  bytes, so hosts decode notifications as a byte stream exactly like serial
  reads. After a disconnect, the host starts with a fresh decoder.

#### Bandwidth

Telemetry shares the connection with HID reports. ZMK asks the computer for a
7.5 to 15 ms connection interval, and the computer decides. A state sample
frame is 59 bytes: one notification once the computer has exchanged a larger
ATT MTU (ZMK supports 65; computers usually exchange it on connect), three at
the minimum MTU of 23.

Notifications on one connection leave in order, HID reports included. To keep
pointing and typing responsive, firmware keeps at most
`CONFIG_ZMK_POINTING_TOOLS_TELEMETRY_BLE_TX_CREDITS` notifications (default 2)
in the Bluetooth stack at once, so a HID report waits behind at most that many.
In a nice!nano ZMK build, a full 65-byte notification leaves as three
link-layer packets (27-byte ACL buffers), which usually fit in one connection
event. Responses wait for room, so a large response, such as a 1 KB target
description of about 17 notifications, takes several connection events. A
state sample that can't start at once is dropped whole and counted in
`dropped-state-records` (status and heartbeat acknowledgements), like a full
queue.

Decision-level telemetry is sparse and fits easily. Every-frame telemetry runs
at whatever rate the link sustains, at most about two samples per connection
event with the default budget, and drops the rest visibly. A calibration run
then reports its dropped samples. Raising the budget trades HID headroom for
telemetry rate.

#### Power

Nobody has measured the battery cost of streaming yet. Connected but idle, the
service sends nothing. While every-frame telemetry streams, the radio sends
extra packets in connection events it would otherwise keep short, and with
ZMK's peripheral latency of 30 the keyboard may skip fewer events. To check:

- Compare battery drain: note the reported battery level, stream every-frame
  telemetry from the tuner for an hour or so of normal use, and compare the
  drop with an hour of the same use without the tuner connected. Battery
  percentages are coarse, so use long runs.
- For a direct number, power the keyboard through a power profiler (for
  example a Nordic Power Profiler Kit II in source mode on the battery
  terminals) and compare average current with the tuner disconnected,
  connected with telemetry off, and streaming every frame.

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
| `0x10` | host → device | Capture control/status | empty to query, or `pipeline-id:u8`; `0xff` stops |
| `0x11` | host → device | Describe capture pipeline | `pipeline-id:u8` |
| `0x81` | device → host | Describe response | described below |
| `0x82` | device → host | Acknowledgement | `state-dropped:u32, capture-dropped:u32` |
| `0x83` | device → host | Tuning targets | described below |
| `0x84` | device → host | Tuning target description | described below |
| `0x85` | device → host | Tuning result | described below |
| `0x86` | device → host | Parameter help | described below |
| `0x87` | device → host | Target metadata | described below |
| `0x88` | device → host | Parameter metadata | described below |
| `0x89` | device → host | Processor-state status | described below |
| `0x8a` | device → host | Pointing devices | described below |
| `0x8b` | device → host | Pointing device description | described below |
| `0x8c` | device → host | Capture status | described below |
| `0x8d` | device → host | Capture pipeline description | described below |
| `0x91` | device → host | Stage-state sample | described below |
| `0x92` | device → host | Capture batch | described below |

### Describe response

```text
protocol-version:u8
```

The describe response is the protocol version handshake; the tuner rejects
every other version. While the host keeps streaming alive, it sends a
heartbeat at least every five seconds. Firmware disables all state levels
automatically when the host disappears.

## Runtime tuning

Targets are runtime instances rather than hard-coded left/right devices. Every
instance of a tunable pipeline stage registers one target at boot, in
devicetree order:

| Stage | Parameters |
| --- | --- |
| `measlesbagel,zpt-stage-coherent-displacement` | `enabled`, `activation-distance`, `coherence`, `qualification-timeout`, `idle-timeout` |
| `measlesbagel,zpt-stage-cursor-transfer` | `scale-multiplier`, `scale-divisor` |
| `measlesbagel,zpt-stage-scroll-batcher` | `steps-per-meter`, `report-interval` |
| `measlesbagel,zpt-stage-text-nav` | `horizontal-threshold`, `vertical-threshold`, `idle-timeout` |
| `measlesbagel,zpt-stage-axis-intent` | `engage-ratio`, `release-ratio`, `activation-distance`, `window`, `idle-timeout` |
| `measlesbagel,zpt-stage-axis-constraint` | `discard-unclassified`, `idle-timeout`, `fold-interval` |
| `measlesbagel,zpt-stage-cursor-acceleration` | `curve-speed-1` to `curve-speed-8`, `curve-sensitivity-1` to `curve-sensitivity-8`, `speed-rise`, `speed-fall` |
| `measlesbagel,zpt-stage-cursor-coalescer` | `usb-report-interval`, `ble-report-interval` |

Values use the stage binding's devicetree units (micrometres, millimetres per
second, steps per metre, milliseconds, percent), and each parameter's devicetree property is the
binding property it came from. Parameters that share a devicetree property are
the elements of that array property, in parameter-id order: the acceleration
curve's point speeds are ids 0 to 7 of `curve-speeds-mm-per-s` and its
sensitivities ids 8 to 15 of `curve-sensitivity-percent`. A host moves a whole
curve in one atomic batch, since firmware rejects point speeds that don't
strictly increase. The target's stable id and label are the
stage's `stable-id`. A change lands between frames: firmware holds every router
while it swaps the stage's settings, so no frame sees half of a batch. Stages
read their settings on every frame, so nothing else is reset. A stage that
doesn't fit in `CONFIG_ZMK_POINTING_TOOLS_TUNING_MAX_TARGETS` logs a warning
and keeps its compiled settings.

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
pipeline stage. Current firmware registers only kind `4`; the others belonged
to the removed monolithic processors.

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
invalid value (`3`), internal error (`4`), and, for pointing-device results
only, unreachable (`5`): the split half that owns the device didn't answer. A
successful preview returns the effective current value.

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

Stable target IDs are keyboard policy supplied by each stage's `stable-id`.
Parameter keys are owned by each stage type. Devicetree paths and property names allow a
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
  location:u8          /* 0 central-local, otherwise peripheral index + 1 */
  flags:u8             /* bit0 reachable, bit1 settable */
  label-length:u8
  label:utf8[label-length]
```

The label is the entry's stable id. Listing never contacts a peripheral.
Central-local entries are always reachable; an entry owned by another half is
reachable when that half answered the last control request sent to it, so it
starts out unreachable until the first describe or preview.

### Describe pointing device

```text
stable-id-length:u8, stable-id:utf8[..]
devicetree-path-length:u16, devicetree-path:utf8[..]
current-cpi:u16
default-cpi:u16       /* compiled devicetree value */
flags:u8              /* bit0 settable (capabilities follow), bit1 stale */
if settable and discrete:
  value-count:u8, then value-count × cpi:u16   /* ascending */
if settable and not discrete:
  reserved:u8 (zero), min:u16, max:u16, step:u16
```

Describing an entry owned by another half reads its resolution live through
the split. If that half doesn't answer within 500 ms, the description still
arrives with the last value the central knows and the stale flag set.

Unknown ids fail with an ordinary tuning result carrying request type `0x0e`
and status unknown target. A description that can't fit the response payload
fails with status internal error.

### Preview device CPI

The tuning result message carries the outcome with request type `0x0f`, the
device id in its parameter-id byte, and the effective value on success:
firmware validates the request against the declared capabilities and snaps to
the nearest supported value when needed, so a host compares requested against
effective to offer corrections. Previews are RAM-only and expire on reboot,
like tuning previews. Read-only devices reject previews with status invalid
value. A preview for a peripheral's device is relayed to that half, and fails
with status unreachable when the half doesn't answer within 500 ms.

Frames from a previewed device are labelled with the new resolution from the
next frame boundary, as long as its source processor names the device (see
[`device-cpi-control.md`](device-cpi-control.md)). Cursor speed then changes
with CPI while millimetre-based thresholds keep their physical meaning.

## Stage-state telemetry

Optional diagnostics are keyed by target id in the shared tuning/telemetry
target table. Tuning-registered targets take the first ids; pipeline stage
observers (`measlesbagel,zpt-pipeline-telemetry`) reuse the tuning id of a
tunable stage and allocate the remaining ids for the rest at init, and the
status response enumerates every allocated target. A stage observed in several
pipelines is one target.

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

The decisions level emits stage decisions only. The every-frame level adds
one sample for every signal the stage emits, with `event` frame (`1`), no
flags, `intent` set to the signal's axis-intent annotation, `timestamp-ms` set
to the signal's observation time (or the processing time when the source left
it unset), and these values:

| Index | Value |
| --- | --- |
| 0 | `128`, marking an every-frame sample |
| 1 | signal kind (`1` raw, `2` normalized, `3` pointer, `4` scroll steps, `5` action) |
| 2, 3 | x and y: counts for raw motion, Q16 millimetres for normalized motion, units for pointer and scroll deltas, the action id and zero for actions |
| 4 | resolution in CPI |
| 5 | source-side sample span in microseconds |
| 6 | source sequence |
| 7 | signal metadata flags (coalesced, timing estimated, clipped, sequence gap, discontinuity, span clipped) |
| 8 | source id |
| 9 | speed annotation in Q16 millimetres per second, zero until a stage sets it |

Values that exceed the i32 range saturate. The tuner's calibration workflow
reads every-frame samples from a resolution-normalize stage (see
[`calibration.md`](calibration.md)).
Reserved values and unknown flags must be ignored for forward compatibility.
Firmware disables all state levels after the ordinary host heartbeat timeout
or an explicit all-targets-off request.

## Input capture

Input capture records what goes into one pipeline: every input frame with its
raw counts, microsecond arrival time, and sensor sample span, the key presses
the keypress guard sees, and the moments stage deadlines run. A host turns a
recording into a [fixture v2 trace](trace-benchmark.md#fixture-format-v2) with
`host/replay/capture.js`, and the trace replays through the firmware's own
stage code (see [Turning a capture into a trace](#turning-a-capture-into-a-trace)).
The stage-state samples above can't do this: they carry millisecond times and
what stages emit, not what went in.

`CONFIG_ZMK_POINTING_TOOLS_INPUT_CAPTURE` is on wherever the pipeline runtime
and the telemetry service are. Capture is off at runtime until a host starts a
recording, and it stops on an explicit stop, on another recording, or after
the ordinary host heartbeat timeout. While nothing records, each frame costs
one atomic read in every recordable pipeline. Every devicetree pipeline is
recordable, in devicetree order, up to
`CONFIG_ZMK_POINTING_TOOLS_INPUT_CAPTURE_MAX_PIPELINES` (default 8).

### Capture control

An empty request queries the status. `pipeline-id` starts recording that
pipeline, ending any recording before it, and `0xff` stops. Firmware sends
every record of the previous recording before the status that answers. A
start is applied between frames, as tuning changes are: firmware resets the
pipeline's stages so they start where replay starts, which drops at most a
sub-pixel remainder and one report interval of pending motion. Every request
receives:

```text
capture-schema-version:u8      /* 1 */
recording:u8                   /* pipeline id, or 0xff when none */
capture-dropped:u32
pipeline-count:u8
repeat pipeline-count times:
  pipeline-id:u8
  active:u8                    /* 1 while the router runs this pipeline */
  stage-count:u8
  label-length:u8
  label:utf8[label-length]     /* the pipeline's stable id, at most 27 bytes */
```

### Capture pipeline description

```text
pipeline-id:u8
stable-id-length:u8, stable-id:utf8[..]
stage-count:u8
repeat stage-count times:
  strategy-length:u8, strategy:utf8[..]      /* e.g. cursor-acceleration */
  stable-id-length:u8, stable-id:utf8[..]
  setting-count:u8
  repeat setting-count times:
    key-length:u8, key:utf8[..]
    value:i32
```

Stages are in pipeline order. A stage's tunable settings come from tuning
discovery under its stable id; the description adds only what tuning doesn't
publish: an orientation's `swap-xy`, `invert-x`, and `invert-y`, and a motion
gate's `suppress-after-keypress-ms` (the keypress guard it references, 0 for
none). Unknown ids fail with an ordinary tuning result carrying request type
`0x11` and status unknown target.

### Capture batches

```text
first-sequence:u32
record-count:u8
repeat record-count times:
  kind:u8, then the kind's fields
```

Records are numbered from zero at the start of each recording, and the
records of a batch are consecutive, so a gap between batches is a drop. A
batch is at most 119 bytes of payload, two notifications at the 65-byte ATT
MTU. Firmware sends a batch when it is full or when the queue is empty.

| Kind | Record | Fields |
| --- | --- | --- |
| `1` | Frame | `flags:u8, time-ms:u32, time-us:u16, x:i32, y:i32, span-us:u32` |
| `2` | Source | `link:u8, source-id:u16, cpi:u16` |
| `3` | Key press | `time-ms:u32` |
| `4` | Entered | none |
| `5` | Left | none |
| `6` | Settings changed | none |
| `7` | Flush | `time-ms:u32` |

- **Frame.** An input frame as the pipeline received it. `time-ms` is the
  uptime millisecond its stages see; `time-us` is how far into that
  millisecond it arrived (999 when it waited past it). `x` and `y` are raw
  counts, saturated to 32 bits, and `span-us` is the sensor sample span, 0
  when the source has none. `flags` are the signal metadata flags bits 0 to 7
  (local, transported, coalesced, timing estimated, clipped, sequence gap,
  discontinuity, span clipped).
- **Source.** Sent before the first frame and whenever the source id,
  resolution, or report link changes. `link` is `0` USB, `1` Bluetooth, or
  `0xff` without a cursor coalescer.
- **Key press.** At the time the keypress guard records. A recording starts
  with the latest key press before it, if there was one.
- **Entered** and **left.** The router activated or deactivated the pipeline;
  entered follows a start too. Stages start from reset after entered.
- **Settings changed.** A tuning change landed; frames after it ran with
  other settings than the host read before recording.
- **Flush.** Stage deadlines that were due ran at `time-ms`, before any frame
  recorded after it.

### Drops

Records travel through the same bounded queue as state samples. A record that
doesn't fit the queue, and a batch the link has no room for (see
[Bandwidth](#bandwidth)), is dropped whole and counted in `capture-dropped`
(capture status and heartbeat acknowledgements), the way `dropped-state-records`
counts state samples. At the 1 ms USB report rate a moving trackball produces
about a thousand frames a second, 20 bytes each; USB serial carries that, and
over Bluetooth expect drops at speed unless the credit budget is raised. A
recording replays only between drops.

### Turning a capture into a trace

Before starting a recording, read the pipeline description and the current
settings of its stages (tuning discovery, or a tuner profile export). Keep
the device's bytes from the description on. Then:

```sh
node host/replay/capture.js --stream capture.bin --profile profile.json \
  --out trace.json --device right-ball
node host/replay/bench.js --fixtures <dir with sets of traces>
```

`capture.js` writes one cursor-pipeline trace per stretch the pipeline ran
unchanged, from an entered record to a left record, a settings change, a
drop, or a change of source, resolution, or link, and warns for each early
end. Event times count from the earliest event. Recorded flushes become
`flush` events, so replay runs deadlines where firmware ran them; a clipped
span becomes a 40 ms span, which restarts the acceleration speed estimate as
the clipped flag does in firmware. Only cursor pipelines convert so far.

The CTest `capture_replay` checks that this is faithful: it records a
synthetic session through the firmware's cursor stages
(`host/tests/capture_session.c`), converts the capture, and requires replay to
send exactly the reports the stages sent.
