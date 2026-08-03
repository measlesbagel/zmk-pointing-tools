export class ResponseRequestQueue {
  #active;
  #onError;
  #queue = [];
  #send;

  constructor(send, onError = () => {}) {
    this.#send = send;
    this.#onError = onError;
  }

  enqueue(responseType, frame) {
    this.#queue.push({ responseType, frame });
    this.#pump();
  }

  complete(responseType) {
    if (!this.#active || this.#active.responseType !== responseType) return false;
    this.#active = undefined;
    this.#pump();
    return true;
  }

  clear() {
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
    try {
      Promise.resolve(this.#send(request.frame)).catch((error) => this.#fail(request, error));
    } catch (error) {
      this.#fail(request, error);
    }
  }

  #fail(request, error) {
    if (this.#active === request) {
      this.#active = undefined;
      this.#onError(error);
      this.#pump();
    }
  }
}
