const USB_FILTERS = [{ usbVendorId: 0x16c0 }];

/* A byte link to the keyboard's tuning protocol. The tuner frames and parses
 * messages itself, so a transport only opens a link, moves bytes, and reports
 * when the link goes away. Every transport has the same shape: label,
 * open(), write(bytes), close(), and the onData, onError and onClose
 * callbacks. */
export class SerialTransport {
  static get supported() {
    return typeof navigator !== "undefined" && "serial" in navigator;
  }

  label = "USB serial";

  constructor({ onData, onError, onClose }) {
    this.onData = onData;
    this.onError = onError;
    this.onClose = onClose;
  }

  /* Asks the user for a port. A failure leaves nothing open. */
  async open() {
    try {
      this.port = await navigator.serial.requestPort({ filters: USB_FILTERS });
      await this.port.open({ baudRate: 115200 });
      this.writer = this.port.writable.getWriter();
    } catch (error) {
      await this.close();
      throw error;
    }
    this.active = true;
    this.unplugged = (event) => {
      if (event.target === this.port) this.onClose();
    };
    navigator.serial.addEventListener("disconnect", this.unplugged);
    this.readLoop();
  }

  write(bytes) {
    return this.writer.write(bytes);
  }

  async readLoop() {
    while (this.active && this.port?.readable) {
      this.reader = this.port.readable.getReader();
      try {
        for (;;) {
          const { value, done } = await this.reader.read();
          if (done) break;
          this.onData(value);
        }
      } catch (error) {
        if (this.active) this.onError(error);
      } finally {
        this.reader.releaseLock();
        this.reader = undefined;
      }
    }
    /* A fatal read error ends the loop without an unplug event. */
    if (this.active) this.onClose();
  }

  async close() {
    this.active = false;
    if (this.unplugged) navigator.serial.removeEventListener("disconnect", this.unplugged);
    try { if (this.reader) await this.reader.cancel(); } catch {}
    try { if (this.writer) this.writer.releaseLock(); } catch {}
    try { if (this.port) await this.port.close(); } catch {}
    this.port = this.writer = undefined;
  }
}

/* GATT service and characteristics from docs/protocol.md (Transports). */
export const BLUETOOTH_SERVICE = "f336b450-531c-49cf-9d5d-846a5db2398b";
export const BLUETOOTH_REQUEST = "f336b451-531c-49cf-9d5d-846a5db2398b";
export const BLUETOOTH_RESPONSE = "f336b452-531c-49cf-9d5d-846a5db2398b";
/* Fits the minimum ATT MTU; firmware doesn't take long writes. */
export const BLUETOOTH_CHUNK = 20;

/* Rewords the browser's GATT errors as what the user can do about them. */
export function bluetoothErrorMessage(error) {
  const text = `${error?.name ?? ""} ${error?.message ?? ""}`;
  if (error?.name === "NotFoundError" && /cancel/i.test(text)) return "No keyboard was chosen.";
  if (/authenticat|encrypt|insufficient/i.test(text)) {
    return "The keyboard refused the connection because this computer isn't bonded with it. Pair the keyboard with this computer in the system Bluetooth settings, then connect again.";
  }
  if (/service|characteristic/i.test(text) && error?.name === "NotFoundError") {
    return "This device doesn't offer the tuning service. Pick the keyboard's central half, and make sure its firmware has the Bluetooth transport (CONFIG_ZMK_POINTING_TOOLS_TELEMETRY_BLE).";
  }
  return error?.message || String(error);
}

/* The keyboard over Web Bluetooth, beside its HID connection. Requests go to
 * the request characteristic in chunks of at most 20 bytes, written with
 * response so a full firmware buffer fails visibly instead of losing bytes.
 * Responses and telemetry arrive as notifications, a byte stream split at
 * arbitrary points like serial reads. */
export class BluetoothTransport {
  static get supported() {
    return typeof navigator !== "undefined" && "bluetooth" in navigator;
  }

  label = "Bluetooth";

  constructor({ onData, onError, onClose, bluetooth = globalThis.navigator?.bluetooth }) {
    this.onData = onData;
    this.onError = onError;
    this.onClose = onClose;
    this.bluetooth = bluetooth;
    this.writes = Promise.resolve();
  }

  /* Asks the user for the keyboard. The tuning service isn't advertised and
   * the keyboard is usually already connected as a keyboard, so the picker
   * lists every device; optionalServices grants access to the service. A
   * failure leaves nothing open. */
  async open() {
    try {
      if (this.bluetooth.getAvailability && !(await this.bluetooth.getAvailability())) {
        throw new Error("Bluetooth is off or this computer has no Bluetooth adapter.");
      }
      this.device = await this.bluetooth.requestDevice({
        acceptAllDevices: true,
        optionalServices: [BLUETOOTH_SERVICE],
      });
      this.dropped = () => { if (this.active) this.onClose(); };
      this.device.addEventListener("gattserverdisconnected", this.dropped);
      const server = await this.device.gatt.connect();
      const service = await server.getPrimaryService(BLUETOOTH_SERVICE);
      this.request = await service.getCharacteristic(BLUETOOTH_REQUEST);
      this.response = await service.getCharacteristic(BLUETOOTH_RESPONSE);
      this.notified = (event) => {
        const view = event.target.value;
        this.onData(new Uint8Array(view.buffer.slice(view.byteOffset, view.byteOffset + view.byteLength)));
      };
      this.response.addEventListener("characteristicvaluechanged", this.notified);
      /* Subscribing needs an encrypted link, so an unbonded computer fails
       * here; the browser may ask to pair first. */
      await this.response.startNotifications();
    } catch (error) {
      await this.close();
      throw new Error(bluetoothErrorMessage(error), { cause: error });
    }
    this.active = true;
  }

  /* Writes one frame. Frames queue behind each other, so two callers never
   * interleave their chunks, and the browser only runs one GATT operation at
   * a time anyway. */
  write(bytes) {
    const chunks = [];
    for (let offset = 0; offset < bytes.length; offset += BLUETOOTH_CHUNK) {
      chunks.push(bytes.slice(offset, offset + BLUETOOTH_CHUNK));
    }
    const done = this.writes.then(async () => {
      for (const chunk of chunks) {
        if (!this.request) throw new Error("The Bluetooth connection is closed");
        try {
          await this.request.writeValueWithResponse(chunk);
        } catch (error) {
          throw new Error(bluetoothErrorMessage(error), { cause: error });
        }
      }
    });
    /* A failed frame doesn't block the frames after it. */
    this.writes = done.catch(() => {});
    return done;
  }

  async close() {
    this.active = false;
    if (this.device && this.dropped) this.device.removeEventListener("gattserverdisconnected", this.dropped);
    if (this.response && this.notified) {
      this.response.removeEventListener("characteristicvaluechanged", this.notified);
      try { await this.response.stopNotifications(); } catch {}
    }
    /* Ends this page's GATT session; the system keeps the keyboard
     * connected for typing and pointing. */
    try { if (this.device?.gatt?.connected) this.device.gatt.disconnect(); } catch {}
    this.device = this.request = this.response = undefined;
  }
}
