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

test("a failed frame rejects and the next frame still goes out", async () => {
  const fake = fakeBluetooth({ writeDelays: [new Error("GATT operation failed")] });
  const link = await open(fake);
  await assert.rejects(link.write(Uint8Array.of(1, 2)), /GATT operation failed/);
  await link.write(Uint8Array.of(3));
  assert.deepEqual(fake.written, [[3]]);
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

test("an unbonded computer gets a pairing hint and nothing stays open", async () => {
  const fake = fakeBluetooth({
    notifyError: Object.assign(new Error("GATT operation failed due to insufficient authentication."), { name: "NetworkError" }),
  });
  await assert.rejects(open(fake), /isn't bonded/);
  assert.equal(fake.device.gatt.connected, false);
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
