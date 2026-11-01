# Host protocol

The protocol runs over USB CDC-ACM serial (Web Serial) and, optionally, over a
Bluetooth GATT service (Web Bluetooth). Both carry the same framed byte stream;
see [Transports](#transports). All multi-byte integers are little-endian.

The current protocol is version 8. Version 8 added input capture and the
capture drop counter in acknowledgements. Firmware and host are updated
together; the tuner rejects every other version instead of carrying
compatibility parsers or feature gates for older firmware.

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
automatically when the host disappears. Acknowledgements always carry both
drop counters, zero for a feature that isn't built.

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

`CONFIG_ZMK_POINTING_TOOLS_INPUT_CAPTURE` defaults on in builds with the
pipeline runtime, the telemetry service, and runtime tuning, and can be
turned off. Capture is off at runtime until a host starts a recording, and it
stops on an explicit stop, on another recording, or after the ordinary host
heartbeat timeout. While nothing records, each frame and each flush that runs
a deadline costs one atomic read in every recordable pipeline. Every
devicetree pipeline is recordable, in devicetree order, up to
`CONFIG_ZMK_POINTING_TOOLS_INPUT_CAPTURE_MAX_PIPELINES` (default 8, at most
12, so the status fits one 512-byte response).

A recording of a pipeline with a keypress guard holds the time of every key
press that reaches the guard while it records, so a trace carries the user's
typing rhythm, though not which keys. Pipelines without a guard record no key
presses.

### Capture control

An empty request queries the status. `pipeline-id` starts recording that
pipeline, ending any recording before it, and `0xff` stops. Firmware sends
every record of the previous recording before the status that answers. A
start is applied between frames, as tuning changes are. When every stage of
the pipeline is one the host replays (the replayable flag below) and the
router runs it, firmware resets its stages so they start where replay starts,
and sends an entered record. The reset is visible: it drops the quantizer's
sub-pixel remainder and up to one report interval of motion the coalescer
holds, returns the motion gate to idle, so motion already moving must
qualify again, and restarts the acceleration speed estimate; in a scroll or
text pipeline it forgets the axis intent, the undecided motion the axis
constraint holds, the scroll batcher's pending motion and sub-step
remainder, and the travel toward the next text step. Pipelines that aren't
replayable are never reset; their recordings start at the next entered
record. A pipeline is replayable when the host knows every stage (the
cursor stages, `axis-intent`, `axis-constraint`, `scroll-batcher`, and
`text-nav`) and the stages that take a keypress guard share one that
capture can freeze. Every request receives:

```text
capture-schema-version:u8      /* 2 */
recording:u8                   /* pipeline id, or 0xff when none */
capture-dropped:u32
buffer-used:u32                /* bytes of records waiting to be sent */
buffer-size:u32                /* the burst buffer's capacity */
pipeline-count:u8
repeat pipeline-count times:
  pipeline-id:u8
  active:u8                    /* 1 while the router runs this pipeline */
  stage-count:u8
  flags:u8                     /* bit 0: replayable */
  settings-hash:u32            /* see the pipeline description */
  label-length:u8
  label:utf8[label-length]     /* the pipeline's stable id, at most 27 bytes */
```

### Capture pipeline description

```text
pipeline-id:u8
stable-id-length:u8, stable-id:utf8[..]
settings-hash:u32
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
publish: an orientation's `swap-xy`, `invert-x`, and `invert-y`; an axis
intent's `policy` (`0` free, `1` adaptive, `2` horizontal, `3` vertical); and
the `suppress-after-keypress-ms` of a motion gate, axis intent, axis
constraint, or scroll batcher (the window of the keypress guard it
references, 0 for none). Unknown ids fail with an ordinary tuning result carrying request type
`0x11` and status unknown target.

`settings-hash` identifies the tuning values the pipeline runs now: the sum,
modulo 2³², of 32-bit FNV-1a over the stage's stable id, a NUL byte, the
parameter key, a NUL byte, and the current value as 4 little-endian bytes,
for every parameter of every stage of the pipeline that has a tuning target.
Order doesn't matter, so a host computes it from a tuner profile export
(`captureSettingsHash` in `web/protocol.js`). Every entered record carries
the hash the pipeline runs from then on, and the converter gives each trace
the profile with that hash.

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
MTU. Batches come from the [burst buffer](#burst-buffer) whenever the link
has room.

Capture schema 2 (this one) added delta frames and the buffer fill in the
status; schema 1 firmware sent every frame whole and had no buffer. The
tools read only schema 2: record schema 1 streams again with current
firmware.

| Kind | Record | Fields |
| --- | --- | --- |
| `1` | Frame | `flags:u8, time-ms:u32, time-us:u16, x:i32, y:i32, span-us:u32` |
| `0x80`–`0x87` | Delta frame | `elapsed:svarint, x:svarint, y:svarint`, then `flags:u8` if bit 2 is set, then the span per bits 0–1 |
| `2` | Source | `link:u8, source-id:u16, cpi:u16` |
| `3` | Key press | `time-ms:u32` |
| `4` | Entered | `settings-hash:u32` |
| `5` | Left | none |
| `6` | Settings changed | none |
| `7` | Flush | `time-ms:u32` |

- **Frame.** An input frame as the pipeline received it. `time-ms` is the
  uptime millisecond its stages see; `time-us` is how far into that
  millisecond it arrived (999 when it waited past it). A frame can carry an
  earlier millisecond than a flush recorded before it, when the deadline ran
  late; replay hands the stages the recorded millisecond (see below). `x` and `y` are raw
  counts, saturated to 32 bits, and `span-us` is the sensor sample span, 0
  when the source has none. `flags` are the signal metadata flags bits 0 to 7
  (local, transported, coalesced, timing estimated, clipped, sequence gap,
  discontinuity, span clipped).
- **Delta frame.** The same frame, relative to the frame before it, in about
  6 bytes. `elapsed` is the time in microseconds since the previous frame's
  `time-ms`·1000 + `time-us` (it can be negative when a frame waited past
  its millisecond), and `x` and `y` are the raw counts. Signed values are
  zigzag-encoded (0, −1, 1, −2, … as 0, 1, 2, 3, …) LEB128 varints of at
  most 5 bytes. Without bit 2 the flags are the previous frame's. Bits 0–1
  give the span: `0` the previous frame's span, `1` equal to `elapsed`, `2`
  an `svarint` difference from `elapsed` follows, `3` the span as an
  unsigned varint follows. The 5th byte of a varint holds only the top 4
  bits (at most `0x0f`), so every signed field fits 32 bits. Firmware sends
  a frame whole at the start of a recording, as the first frame after a
  drop and after every entered record, and when the delta wouldn't be
  shorter. A host that meets a delta frame with no frame to follow (after a
  sequence gap or an entered record) skips it and every delta after it
  until the next whole frame, and treats the skipped records as a gap.
  Delta frames carry everything a whole frame does, exactly.
- **Source.** Sent before the first frame and whenever the source id,
  resolution, or report link changes; a change makes firmware resync at the
  next rest. `link` is `0` USB, `1` Bluetooth, or
  `0xff` without a cursor coalescer.
- **Key press.** The latest key press the pipeline's keypress guard holds,
  at the time it holds, recorded before the first frame or flush it
  decides. The guard is read once per frame and once per flush and holds
  that view until the frame or flush is done, so a key press landing while
  one runs counts from the next, in firmware and in replay alike. Flushes
  need this too: the axis constraint's deadline hands the motion it held to
  the scroll batcher, which asks the guard. The first frame of a recording records the
  guard's key press from before the recording; only a guard that capture can
  freeze this way is recorded.
- **Entered** and **left.** The router activated or deactivated the pipeline;
  entered also follows a start and a resync (see [Drops and resyncs](#drops-and-resyncs)). Stages
  start from reset after entered, with the settings `settings-hash`
  identifies.
- **Settings changed.** A tuning change landed, such as a preview while
  recording; frames after it ran with other settings. Firmware resyncs at
  the next rest.
- **Flush.** Stage deadlines that were due ran at `time-ms`, before any frame
  recorded after it.

### Burst buffer

A sensor read every millisecond while the ball moves makes about a thousand
frames a second, whatever the cursor report rate; over Bluetooth, with the
default two notifications per connection event, the link carries about 120
bytes per event (see [Bandwidth](#bandwidth)). So firmware stores and
forwards: each record is encoded as it is recorded, into a RAM ring of
`CONFIG_ZMK_POINTING_TOOLS_INPUT_CAPTURE_BUFFER_SIZE` bytes (default 4096,
about 680 frames; at least 512), and the protocol worker sends batches from
it whenever the link has room, mostly in the rests between strokes. Raise
the option for long bursts on slow links; each byte is static RAM whether
or not a recording runs. Only the average record rate has to fit the
link, not a flick's peak. The buffer changes when records leave, never what
the pipeline does. `buffer-used` in the capture status shows how full it is.
Stopping a recording sends everything stored, waiting for the link, before
the status that confirms the stop; if the link fails meanwhile, what is left
is dropped and counted. A full default buffer takes about 0.25 s to drain on
a 7.5 ms connection interval and about 1 s at 30 ms (two notifications per
event); a buffer raised to 16 KB takes about 4 s at 30 ms, close to the
tuner's 5 s request timeout. Capture records don't use the state-sample
queue. Records are stored with interrupts masked for a few hundred cycles
per record; the worker copies a batch out with interrupts enabled, and
skips building one while the Bluetooth link has no notification credit.

In the host test's simulated link (`capture_replay`), frames average about
5.7 bytes; two credits per 7.5 ms connection event keep up with fast flicks
read every half millisecond, and at 15 ms the buffer holds the bursts with
under 1 KB in use (also run with a 1 KB buffer, so the ring wraps without
drops). The simulation is generous: every connection event refills both
credits, and HID reports never share the connection. These are
simulations, not hardware measurements.

### Drops and resyncs

A record that doesn't fit the burst buffer is dropped and counted in
`capture-dropped` (capture status and heartbeat acknowledgements), the way
`dropped-state-records` counts state samples. Records are numbered whether or
not they fit, so the host sees every drop as a sequence gap.

After a drop, replay can't know what the stages held, so the trace ends; the
same goes for a settings change, a CPI preview, and a switch of report link.
The pipeline keeps running unchanged, since resetting it mid-motion would
change how the cursor feels. Firmware resyncs at the next rest instead: at the first
frame after at least 40 ms without a frame or flush (the acceleration
stage's restart gap, or longer if a stage remembers motion longer: the
coalescer's or scroll batcher's report interval, or the idle timeout of the
axis intent, axis constraint (or its fold interval), or text navigation) and
with no stage deadline pending, it resets the pipeline and sends an entered
record, and a new trace starts there. By then the motion gate is idle with
nothing pending, the coalescer holds nothing and its interval has passed,
and the acceleration stage restarts its speed estimate unless the next frame
carries a sensor span shorter than 40 ms. The reset therefore changes only
what a rest leaves behind: the quantizer's sub-pixel remainder (less than
one count) and, for such a frame, the speed estimate the stage would have
blended it into. In a scroll or text pipeline the reset clears the scroll
batcher's sub-step remainder and the axis intent estimator; after the rest
the estimator would have started over anyway unless its idle timeout is 0
(never forgets), so only the remainder and that case differ from firmware
that never resynced. Frames between the drop or change and the resync don't
replay. Only replayable pipelines resync; for others the recording replays
up to the first drop or change.

### Turning a capture into a trace

Before starting a recording, read the pipeline description and the current
settings of its stages (tuning discovery, or a tuner profile export), and
export a profile again after every settings change while recording. Keep
the device's bytes from the description on. Then:

```sh
node host/replay/capture.js --stream capture.bin --profile a.json \
  --profile b.json --out trace.json --device right-ball
node host/replay/bench.js --fixtures <dir with sets of traces>
```

`capture.js` writes one trace per stretch the pipeline ran
unchanged, from an entered record to a left record, a settings change, a
drop, or a change of source, resolution, or link, and warns for each early
end and for a recording whose first records were dropped. Each trace uses
the profile whose settings hash its entered record carries; a trace no
profile matches is skipped with a warning. Event times count
from the earliest event, and `metadata.capture.startMs` holds the device
uptime of time zero. Recorded flushes become `flush` events, so replay runs
deadlines where firmware ran them. An event recorded after a later one, such
as a frame after a late flush, moves to that one's time and carries how many
milliseconds earlier its stages saw it as a trailing `late` field (see
[`trace-benchmark.md`](trace-benchmark.md#fixture-format-v2)), so replay hands
every stage the millisecond firmware did. Key presses older than the
guard's `suppress-after-keypress-ms` when the trace starts are left out, and
so are all key presses for a pipeline without a guard. A clipped span
becomes a 40 ms span, which restarts the acceleration speed estimate as the
clipped flag does in firmware. A pipeline ending in a scroll batcher becomes
a `scroll-pipeline` trace and one ending in text navigation a
`text-pipeline` trace; ground truth comes from optional labels and, on request, from the motion itself (see
[`trace-benchmark.md`](trace-benchmark.md#recorded-traces-ground-truth));
every other pipeline becomes a `cursor-pipeline` trace.

The CTest `capture_replay` checks that this is faithful: it records a
synthetic session through the firmware's cursor stages and capture tap
(`host/tests/capture_session.c`), with flushes running up to 3 ms late, key
presses landing while a frame runs, a tuning change mid-stroke, and fast
flicks, through the burst buffer and a simulated Bluetooth link. It runs at
a 7.5 ms and a 15 ms connection interval, where the flicks outrun the link,
and with a 512-byte buffer that fills and drops records; every record must
arrive in order or show as a counted gap, and the tap resyncs after each
change and drop. It converts the capture with both profiles and
requires each trace's replay to send exactly the reports the stages sent
during it. `capture_replay_scroll` and `capture_replay_text` do the same
through the firmware's axis intent, axis constraint, and scroll batcher or
text navigation under one keypress guard, with key presses also landing
while a flush runs; their `_full_buffer` variants drop records, and
`capture_replay_text_horizontal` locks the axis so the recorded policy has
to reach replay. The ztest `capture_records_pipeline_input` checks that the
telemetry service starts a recording with the same records, keeps and
resends batches the link refuses, counts a refused full batch, and resyncs
after a drop and after a settings change.
