# Pointing device discovery and split-safe CPI control

Issue #13. The web tuner can discover the physical pointing devices on a
keyboard, show what each sensor supports, and preview a new resolution on the
half that owns the sensor. Previews live in RAM only and reboot restores the
compiled values. Nothing assumes continuous CPI steps or a particular driver.

```text
web tuner
   │  USB CDC, protocol v7 (docs/protocol.md)
   ▼
telemetry service (central)
   │  device table lookup
   ├─► local entry ──► sensor facet ──► local sensor driver
   │
   └─► peripheral entry ──► proxy ── INVOKE_BEHAVIOR ──► agent (peripheral)
                              ▲                            │ device table
                              │                            ▼
                              └── zmk,input-split ◄── packed response event

source processor (central) ◄── reads the entry's current CPI before each frame
```

There are four layers:

1. **Device table.** Devicetree entries with stable ids, the owning half, and
   capability data, compiled into every half.
2. **Sensor control facet.** An optional driver API. Without it an entry is
   still discoverable, and previews only change the RAM store.
3. **Split tunnel.** The central relays requests for a peripheral's devices to
   an agent behavior on that half and correlates the answers.
4. **Source labelling.** Source processors follow their device's current
   resolution, so normalization stays physically correct after a preview.

## 1. Device table

One `measlesbagel,zpt-pointing-device` node per physical device:

```dts
left_ball: left_ball {
    compatible = "measlesbagel,zpt-pointing-device";
    stable-id = "left-ball";
    location = <1>;               /* 0 central, else peripheral index + 1 */
    resolution-cpi = <684>;
    cpi-min = <608>;              /* or a discrete cpi-values list */
    cpi-max = <4826>;
    cpi-step = <38>;
};
```

- `stable-id` is the host-facing identity. Numeric device ids are dense
  session ids in devicetree order, so every half must compile the same
  entries in the same order.
- Declare either an ascending `cpi-values` list or the
  `cpi-min`/`cpi-max`/`cpi-step` range. Declaring neither makes the device
  read-only. The build rejects mixed or malformed declarations.
- Capabilities come from devicetree rather than probing. Write them from the
  driver's behavior, not the datasheet's marketing range: the PAW3222 driver
  applies multiples of 38 CPI from 608 to 4826, so a board's
  `res-cpi = <700>` really runs at 684.
- `location` is the owning half: 0 for the central, otherwise the peripheral's
  split source index plus one. With one peripheral that is always 1.

### Sensors on a split keyboard

`sensor` is optional because each half only defines its own sensor node.
Declare the entries in devicetree both halves share, then add the sensor from
the owning half's file:

```dts
/* bridges_v2_left.dts (the peripheral owns the left ball) */
&left_ball { sensor = <&trackball_peripheral>; };
```

An entry without a sensor still lists, labels motion, and relays requests. It
just never reaches a driver on that half.

## 2. Sensor control facet

```c
struct zpt_sensor_control_api {
    int (*get_cpi)(const struct device *dev, uint16_t *cpi);
    int (*set_cpi)(const struct device *dev, uint16_t cpi); /* RAM only */
    int (*supported_count)(const struct device *dev, size_t *count);
    int (*supported_get)(const struct device *dev, size_t index, uint16_t *cpi);
};
```

Drivers register the facet against their device with
`zpt_sensor_control_register()`. Callbacks may be NULL: without `get_cpi` the
control layer reports its stored value. `set_cpi` receives values already
validated and snapped against the declared capabilities and must never write
sensor NVM.

The bundled PAW3222 adapter (`CONFIG_ZMK_POINTING_TOOLS_SENSOR_CONTROL_PAW32XX`)
binds every entry whose sensor is a `pixart,paw3222` to the driver's
`paw32xx_set_resolution()`. The driver has no read-back, so reads come from
the store.

The table keeps each entry's current value in RAM, seeded from
`resolution-cpi`. Local previews update it after the facet accepts a value.
For a peripheral's entry, the central records the value the peripheral echoes
back.

## 3. Split tunnel

The central sends requests with ZMK's existing `INVOKE_BEHAVIOR` split
command, so no ZMK changes are needed. Peripherals answer through an ordinary
`zmk,input-split` channel.

### Wiring

Shared devicetree, compiled into every half:

```dts
zpt_cpi: zpt_cpi {
    compatible = "measlesbagel,zpt-sensor-control";
    #binding-cells = <0>;
};

split_inputs {
    #address-cells = <1>;
    #size-cells = <0>;

    /* Use a reg no other split input uses. */
    zpt_cpi_split: zpt_cpi_split@1 {
        compatible = "zmk,input-split";
        reg = <1>;
        device = <&zpt_cpi>;
    };
};

zpt_cpi_proxy {
    compatible = "measlesbagel,zpt-sensor-control-proxy";
    agent = <&zpt_cpi>;
    input = <&zpt_cpi_split>;
};
```

With `CONFIG_ZMK_POINTING_TOOLS_SENSOR_TUNNEL` (on by default when the agent
node exists), peripherals build the agent and the central builds the proxy.
The proxy listens on the split device directly, so no input listener is
needed. Two rules are checked at build time:

- The agent's node name must be at most 8 characters, because ZMK's split
  relay truncates longer behavior names and the peripheral would never find
  it.
- Some `zmk,input-split` node must name the agent as its device. Without one
  the peripheral never forwards answers and every request times out.

### Wire format

```text
request   param1: seq:u4 << 16 | opcode:u8 << 8 | device-id:u8
          param2: cpi:u16 for SET_CPI, otherwise zero
response  one INPUT_REL_MISC event value:
          tag 0xA (31..28) | errno:u8 (27..20) | seq:u4 (19..16) | value:u16
```

Opcodes are `GET_CPI` (1) and `SET_CPI` (2). Sequence numbers run 1 to 15 and
never 0, which marks no outstanding request.

### Behavior

- One request is outstanding at a time, matching the host protocol's flow
  control. The proxy correlates answers by sequence number and drops untagged,
  stale, duplicate, and late frames.
- The peripheral serves requests on the system work queue rather than the
  Bluetooth receive path, because serving talks to the sensor over SPI and
  answering sends a notification.
- The agent only serves entries with a nonzero location. A request for a
  central-owned or unknown entry is answered with `-ENODEV` so the central
  doesn't wait.
- ZMK queues a split command and silently drops it when the half is
  disconnected, so the only sign of an unreachable half is silence. Requests
  time out after 500 ms and report status unreachable. The device list shows a
  peripheral's entries as reachable when that half answered the last request.
- Describing a peripheral's entry reads it live. On a timeout the description
  carries the last known value with the stale flag.

## 4. Source labelling

A source processor that names its device takes the compiled resolution from
the entry and then follows the entry's current value:

```dts
left_source: left_source {
    compatible = "measlesbagel,zpt-input-processor-source";
    #input-processor-cells = <0>;
    source-id = <2>;
    device = <&left_ball>;
    router = <&pointer_router>;
    transported;
    compact-event-code = <INPUT_REL_Z>;
};
```

Before each event the processor compares the entry's current value with the
resolution its next frame will carry. A change applies at once between frames,
or after the frame in progress, so no frame mixes counts from two resolutions.
Cursor speed then changes with CPI, while the motion gate's activation
distance, scroll steps per millimetre, and text thresholds keep their physical
meaning. Without `device`, the source keeps its compiled `resolution-cpi`.

## Known limits

- **Peripheral reboot.** A rebooted peripheral restores its compiled CPI, but
  the central keeps labelling that half's frames with the last previewed value
  until the next describe or preview of that device refreshes it.
- **Change boundary.** The sensor switches resolution when the driver writes
  it, which isn't synchronized with the frame boundary, and transported frames
  already in flight were measured at the old value. Expect at most a frame or
  two at the wrong scale right after a preview.
- **More than one peripheral.** Everything is a list end to end and requests
  name their half, but only one-peripheral splits have been built. A shared
  agent split channel across several peripherals, and the mapping from
  location to ZMK's split source index, need checking on real hardware first.
- **Persistence.** Previews are RAM-only. Exporting a chosen value back into
  devicetree is the way to keep it (#24 owns lifecycle).

## Testing

- **Host tests.** Capability validation and snapping (`device_caps_test`), the
  request/response correlation core including sequence wrap and late answers
  (`sensor_control_session_test`), and resolution staging in the source path
  (`source_normalization_test`, `source_ingress_test`).
- **ztest (`tests/unit`).** Device table discovery, optional sensors, the RAM
  store, recording a peripheral's value, the peripheral's request service, and
  the v7 list, describe and preview messages.
- **Smoke builds.** CI builds both Corne halves with the tunnel wired as above,
  so the central's proxy and the peripheral's agent both compile and link.
- **On hardware.** Round-trip time while the peripheral streams motion at full
  rate is still unmeasured. Check that previews fit the 500 ms budget on the
  first keyboard build.
