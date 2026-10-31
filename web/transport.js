const USB_FILTERS = [{ usbVendorId: 0x16c0 }];

/* A byte link to the keyboard's tuning protocol. The tuner frames and parses
 * messages itself, so a transport only opens a link, moves bytes, and reports
 * when the link goes away. Web Bluetooth (#105) will implement the same
 * shape: label, open(), write(bytes), close(), and the onData, onError and
 * onClose callbacks. */
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
