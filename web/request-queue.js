export class ResponseRequestQueue {
  #active;
  #onError;
  #queue = [];
  #send;
  #timeoutMs;
  #onTimeout;
  #timer;

  /* With timeoutMs, a request whose response never arrives ends the
   * session: the queue drops everything and calls onTimeout. It never moves
   * on to the next request, because a late response could then confirm a
   * request it doesn't belong to. */
  constructor(send, onError = () => {}, { timeoutMs = 0, onTimeout = () => {} } = {}) {
    this.#send = send;
    this.#onError = onError;
    this.#timeoutMs = timeoutMs;
    this.#onTimeout = onTimeout;
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
      this.#timer = setTimeout(() => {
        if (this.#active !== request) return;
        this.clear();
        this.#onTimeout();
      }, this.#timeoutMs);
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
