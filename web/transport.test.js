import assert from "node:assert/strict";
import test from "node:test";

import {
  BLUETOOTH_REQUEST,
  BLUETOOTH_RESPONSE,
  BLUETOOTH_SERVICE,
  BluetoothTransport,
} from "./transport.js";

/* A Web Bluetooth stand-in: one device with the tuning service. Writes are
 * recorded and can be held open to check ordering. */
function fakeBluetooth({ notifyError, writeDelays = [] } = {}) {
  const written = [];
  const response = new EventTarget();
  response.startNotifications = async () => { if (notifyError) throw notifyError; };
  response.stopNotifications = async () => {};
  const request = {
    async writeValueWithResponse(chunk) {
      const delay = writeDelays.shift() ?? 0;
      if (delay instanceof Error) throw delay;
      await new Promise((resolve) => setTimeout(resolve, delay));
      written.push([...chunk]);
    },
  };
  const device = new EventTarget();
  device.gatt = {
    connected: false,
    async connect() {
      this.connected = true;
      return {
        async getPrimaryService(uuid) {
          assert.equal(uuid, BLUETOOTH_SERVICE);
          return {
            async getCharacteristic(id) {
              if (id === BLUETOOTH_REQUEST) return request;
              if (id === BLUETOOTH_RESPONSE) return response;
              throw new Error(`unknown characteristic ${id}`);
            },
          };
        },
      };
    },
    disconnect() { this.connected = false; },
  };
  const bluetooth = {
    options: undefined,
    async getAvailability() { return true; },
    async requestDevice(options) { this.options = options; return device; },
  };
  return { bluetooth, device, response, written };
}

function open(fake, events = {}) {
  const link = new BluetoothTransport({
    onData: events.onData ?? (() => {}),
    onError: () => {},
    onClose: events.onClose ?? (() => {}),
    bluetooth: fake.bluetooth,
  });
  return link.open().then(() => link);
}

test("requests the tuning service and writes frames in 20-byte chunks without interleaving", async () => {
  /* The first frame's chunks finish slowly; the second frame, written
   * meanwhile, must still follow all of them. */
  const fake = fakeBluetooth({ writeDelays: [15, 15, 15] });
  const link = await open(fake);
  assert.deepEqual(fake.bluetooth.options.optionalServices, [BLUETOOTH_SERVICE]);

  const first = Uint8Array.from({ length: 45 }, (_, i) => i);
  const second = Uint8Array.from([200, 201, 202]);
  await Promise.all([link.write(first), link.write(second)]);

  assert.deepEqual(fake.written.map((chunk) => chunk.length), [20, 20, 5, 3]);
  assert.deepEqual(fake.written.flat(), [...first, ...second]);
});

const busy = () => new Error("GATT operation failed for unknown reason.");

test("a rejected chunk is retried in place, so the frame arrives whole", async () => {
  /* Chunk 2 of 3 is rejected once, as firmware does while its buffer is
   * full. */
  const fake = fakeBluetooth({ writeDelays: [0, busy(), 0, 0] });
  let closes = 0;
  const link = await open(fake, { onClose: () => { closes += 1; } });
  const frame = Uint8Array.from({ length: 45 }, (_, i) => i);
  await link.write(frame);
  await link.write(Uint8Array.of(99));
  assert.deepEqual(fake.written.map((chunk) => chunk.length), [20, 20, 5, 1]);
  assert.deepEqual(fake.written.flat(), [...frame, 99]);
  assert.equal(closes, 0);
});

test("a frame that keeps failing mid-way closes the link and sends nothing more", async () => {
  const fake = fakeBluetooth({ writeDelays: [0, busy(), busy(), busy(), busy()] });
  let closes = 0;
  const link = await open(fake, { onClose: () => { closes += 1; } });
  await assert.rejects(link.write(Uint8Array.from({ length: 45 })), /mid-frame/);
  await assert.rejects(link.write(Uint8Array.of(99)), /mid-frame/);
  assert.deepEqual(fake.written.map((chunk) => chunk.length), [20]);
  assert.equal(closes, 1);
});

test("a frame that fails before its first byte doesn't block the next", async () => {
  const fake = fakeBluetooth({ writeDelays: [busy(), busy(), busy(), busy()] });
  let closes = 0;
  const link = await open(fake, { onClose: () => { closes += 1; } });
  await assert.rejects(link.write(Uint8Array.of(1, 2)), /unknown reason/);
  await link.write(Uint8Array.of(3));
  assert.deepEqual(fake.written, [[3]]);
  assert.equal(closes, 0);
});

test("passes notification bytes on, honouring the view's offset", async () => {
  const fake = fakeBluetooth();
  const received = [];
  await open(fake, { onData: (bytes) => received.push([...bytes]) });
  const buffer = Uint8Array.of(9, 9, 0x5a, 0x50, 0x81, 9).buffer;
  const event = new Event("characteristicvaluechanged");
  Object.defineProperty(event, "target", { value: { value: new DataView(buffer, 2, 3) } });
  fake.response.dispatchEvent(event);
  assert.deepEqual(received, [[0x5a, 0x50, 0x81]]);
});

test("reports a dropped link once, and not after the tuner closed it", async () => {
  const fake = fakeBluetooth();
  let closes = 0;
  const link = await open(fake, { onClose: () => { closes += 1; } });
  fake.device.dispatchEvent(new Event("gattserverdisconnected"));
  assert.equal(closes, 1);

  await link.close();
  assert.equal(fake.device.gatt.connected, false);
  fake.device.dispatchEvent(new Event("gattserverdisconnected"));
  assert.equal(closes, 1);
  await assert.rejects(link.write(Uint8Array.of(1)), /closed/);
});

/* Chromium's BluetoothError.cpp wording for an unbonded or unencrypted link. */
for (const [name, message] of [["NetworkError", "GATT Error: Not paired."], ["SecurityError", "GATT operation not authorized."]]) {
  test(`an unbonded computer gets a pairing hint (${message})`, async () => {
    const fake = fakeBluetooth({ notifyError: Object.assign(new Error(message), { name }) });
    await assert.rejects(open(fake), /isn't bonded/);
    assert.equal(fake.device.gatt.connected, false);
  });
}

test("a cancelled picker mentions Bluetooth being off", async () => {
  const fake = fakeBluetooth();
  fake.bluetooth.requestDevice = async () => {
    throw Object.assign(new Error("User cancelled the requestDevice() chooser."), { name: "NotFoundError" });
  };
  await assert.rejects(open(fake), /No keyboard was chosen.*Bluetooth is off/);
});

test("firmware without the service names the Kconfig option", async () => {
  const fake = fakeBluetooth();
  const device = await fake.bluetooth.requestDevice({});
  device.gatt.connect = async () => ({
    async getPrimaryService() {
      throw Object.assign(new Error(`No Services matching UUID ${BLUETOOTH_SERVICE} found in Device.`), { name: "NotFoundError" });
    },
  });
  await assert.rejects(open(fake), /CONFIG_ZMK_POINTING_TOOLS_TELEMETRY_BLE/);
});
