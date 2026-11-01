export class ResponseRequestQueue {
  #active;
  #onError;
  #queue = [];
  #send;
  #timeoutMs;
  #timer;

  /* With timeoutMs, a request whose response never arrives fails with an
   * error instead of holding every later request forever. */
  constructor(send, onError = () => {}, timeoutMs = 0) {
    this.#send = send;
    this.#onError = onError;
    this.#timeoutMs = timeoutMs;
  }

  enqueue(responseType, frame) {
    this.#queue.push({ responseType, frame });
    this.#pump();
  }

  complete(responseType) {
    if (!this.#active || this.#active.responseType !== responseType) return false;
    clearTimeout(this.#timer);
    this.#active = undefined;
    this.#pump();
    return true;
  }

  clear() {
    clearTimeout(this.#timer);
    this.#active = undefined;
    this.#queue = [];
  }

  get pending() {
    return this.#queue.length + Number(Boolean(this.#active));
  }

  #pump() {
    if (this.#active || this.#queue.length === 0) return;
    const request = this.#queue.shift();
    this.#active = request;
    if (this.#timeoutMs > 0) {
      this.#timer = setTimeout(
        () => this.#fail(request, new Error(`No response from the keyboard within ${this.#timeoutMs / 1000} s`)),
        this.#timeoutMs,
      );
    }
    try {
      Promise.resolve(this.#send(request.frame)).catch((error) => this.#fail(request, error));
    } catch (error) {
      this.#fail(request, error);
    }
  }

  #fail(request, error) {
    if (this.#active === request) {
      clearTimeout(this.#timer);
      this.#active = undefined;
      this.#onError(error);
      this.#pump();
    }
  }
}
