/* Lets the user hand the tuner's agent tools to an AI agent (#115). Allowing
 * access registers the agent tools with the browser's WebMCP model context
 * and opens the local relay's widget, which forwards them to a coding agent
 * running @mcp-b/webmcp-local-relay on this computer. Turning access off
 * unregisters the tools and closes the widget but leaves values as they
 * are. The revert point and snapshots belong to the connection, not to one
 * access session, so they outlast turning access off and on.
 *
 * The widget is the relay's own, vendored and pinned. It is loaded here
 * rather than with the relay's embed.js, which reloads the page when the
 * relay asks; a reload would drop the keyboard connection. */

import { AgentSession, describeChanges } from "./agent-tools.js";
import { describeRun } from "./agent-experiments.js";

const RELAY_VERSION = "6.0.3";
const RELAY_PORT = "9333";
const WIDGET_URL = new URL("./vendor/webmcp-local-relay/widget.html", import.meta.url).href;
const TAB_ID_KEY = "__webmcp_relay_tab_id";
const LOG_LIMIT = 50;

/* Only document.modelContext: the relay widget reads tools there, and drops
 * the string schemas older WebMCP builds returned under navigator. */
export function modelContext() {
  return globalThis.document?.modelContext;
}

function relayTabId() {
  try {
    const saved = sessionStorage.getItem(TAB_ID_KEY);
    if (saved) return saved;
    const id = crypto.randomUUID();
    sessionStorage.setItem(TAB_ID_KEY, id);
    return id;
  } catch {
    return crypto.randomUUID();
  }
}

export function relayConfig(origin) {
  return JSON.stringify({
    mcpServers: {
      "zmk-tuner": {
        command: "npx",
        args: ["-y", `@mcp-b/webmcp-local-relay@${RELAY_VERSION}`, "--widget-origin", origin],
      },
    },
  }, null, 2);
}

export class AgentBridge {
  constructor(root, { context, notice, openRun, endRun }) {
    this.context = context;
    this.notice = notice;
    this.openRun = openRun;
    this.endRun = endRun;
    this.log = [];
    this.session = new AgentSession(context, () => this.render());
    this.toggle = root.querySelector("[data-agent-toggle]");
    this.status = root.querySelector("[data-agent-status]");
    this.logList = root.querySelector("[data-agent-log]");
    this.config = root.querySelector("[data-agent-config]");
    this.revertButton = root.querySelector("[data-agent-revert]");
    this.snapshotList = root.querySelector("[data-agent-snapshots]");
    this.questionList = root.querySelector("[data-agent-questions]");
    this.runList = root.querySelector("[data-agent-runs]");
    this.windowList = root.querySelector("[data-agent-windows]");
    this.runList.addEventListener("click", (event) => {
      const id = event.target.closest("[data-run]")?.dataset.run;
      const run = id && this.session.runs.runs.get(id);
      if (!run) return;
      if (event.target.closest("[data-run-open]")) {
        if (run.status === "requested") {
          this.session.runs.accept(id);
          this.record(`Accepted ${describeRun(run)}`);
        }
        this.openRun(run);
      } else if (event.target.closest("[data-run-decline]")) {
        const running = run.status === "running";
        this.session.runs.decline(id);
        /* Ends a calibration still recording; the run no longer counts. */
        if (running) this.endRun(run);
        this.record(`${running ? "Cancelled" : "Declined"} ${describeRun(run)}`);
        /* The card is gone: keep focus in the panel. */
        this.status.focus();
      }
    });
    this.windowList.addEventListener("click", async (event) => {
      const id = event.target.closest("[data-window-stop]")?.dataset.windowStop;
      if (!id) return;
      const stopping = this.session.windows.stop(id, "stopped by you");
      this.status.focus();
      const window = await stopping;
      this.record(`Stopped telemetry window "${window.label}"`);
    });
    this.revertButton.addEventListener("click", () => this.revert());
    this.snapshotList.addEventListener("click", (event) => {
      const button = event.target.closest("[data-snapshot]");
      if (button) this.applySnapshot(button.dataset.snapshot);
    });
    this.questionList.addEventListener("click", (event) => {
      const choice = event.target.closest("[data-choice]");
      const dismiss = event.target.closest("[data-dismiss]");
      const id = event.target.closest("[data-question]")?.dataset.question;
      if (choice) this.answer(id, choice.dataset.choice);
      else if (dismiss) {
        this.session.dismiss(id);
        this.record("Dismissed a question");
      }
    });
    this.questionList.addEventListener("submit", (event) => {
      event.preventDefault();
      const input = event.target.querySelector("input");
      if (input.value.trim()) this.answer(event.target.closest("[data-question]").dataset.question, input.value.trim());
    });
    this.config.textContent = relayConfig(location.origin);
    root.querySelector("[data-agent-copy]").addEventListener("click", async () => {
      try {
        await navigator.clipboard.writeText(this.config.textContent);
        this.notice("Copied the agent's MCP config.");
      } catch (error) {
        this.notice(`Could not copy the MCP config: ${error.message}`, true);
      }
    });
    this.toggle.addEventListener("click", () => (this.controller ? this.disable() : this.enable()));
    this.render();
  }

  async enable() {
    const context = modelContext();
    if (!context || this.controller) return;
    const controller = new AbortController();
    this.controller = controller;
    this.toggle.disabled = true;
    try {
      for (const tool of this.session.tools) {
        await context.registerTool({ ...tool, execute: (input) => this.call(tool, input) },
          { signal: controller.signal });
      }
      await this.openRelay();
      this.notice("Agent access is on. The agent can preview settings; Revert to pre-agent values undoes them.");
    } catch (error) {
      this.disable();
      this.notice(`Could not turn on agent access: ${error.message}`, true);
    } finally {
      this.toggle.disabled = false;
      this.render();
    }
  }

  disable() {
    this.controller?.abort();
    this.controller = undefined;
    this.widget?.remove();
    this.widget = undefined;
    this.session.close();
    if (this.session.changed) this.notice("Agent access is off. The agent's changes stay until you revert them.");
    this.render();
  }

  #add(entry) {
    this.log.unshift({ at: new Date(), ok: true, ...entry });
    this.log.length = Math.min(this.log.length, LOG_LIMIT);
    return this.log[0];
  }

  /* The user's own actions in this panel, logged next to the agent's. */
  record(text) {
    this.#add({ name: "you", kind: "user", text });
    this.render();
  }

  /* A thrown error reaches the agent only as Chrome's generic "invocation
   * failed", so failures come back as an MCP error result, which the relay
   * widget passes on as it is. */
  async call(tool, input) {
    const entry = this.#add({ name: tool.name, kind: tool.annotations.readOnlyHint ? "read" : "change" });
    try {
      const result = await tool.execute(input ?? {});
      entry.text = JSON.parse(result).summary;
      return result;
    } catch (error) {
      entry.ok = false;
      entry.error = String(error?.message ?? error);
      return JSON.stringify({ content: [{ type: "text", text: entry.error }], isError: true });
    } finally {
      this.render();
    }
  }

  async revert() {
    this.revertButton.disabled = true;
    try {
      const { changes, remaining, confirmed } = await this.session.revert();
      this.record(`Reverted to pre-agent values: ${describeChanges(changes)}`);
      if (!confirmed) {
        this.notice("The keyboard hasn't confirmed every reverted value yet. Check the values, then revert again if needed.", true);
      } else if (remaining.length) {
        this.notice(`Some values didn't revert: ${describeChanges(remaining)}. Try again.`, true);
      } else {
        this.notice("Restored the values from before the agent's first change.");
      }
    } catch (error) {
      this.notice(`Could not revert: ${error.message}`, true);
    } finally {
      this.revertButton.disabled = false;
      this.render();
    }
  }

  async applySnapshot(name) {
    try {
      const { changes } = await this.session.applySnapshot(name, { byUser: true });
      this.record(`Applied snapshot "${name}": ${describeChanges(changes)}`);
    } catch (error) {
      this.notice(`Could not apply snapshot "${name}": ${error.message}`, true);
    }
  }

  /* The playground or calibration panel started or ended a run. Runs
   * nobody asked for change nothing. */
  runStarted(kind, activity) {
    this.session.runs.started(kind, activity);
  }

  runEnded(kind, activity, outcome) {
    const run = this.session.runs.ended(kind, activity, outcome);
    if (run) this.record(`${run.status === "finished" ? "Finished" : "Stopped"} ${describeRun(run)}`);
  }

  answer(id, text) {
    this.session.answer(id, text);
    this.record(`Answered: ${text}`);
  }

  /* A hidden blob iframe inherits this page's origin, so the widget can read
   * the page's tools; the page passes it the relay settings embed.js
   * would. */
  async openRelay() {
    const response = await fetch(WIDGET_URL);
    if (!response.ok) throw new Error(`relay widget request failed with status ${response.status}`);
    const host = new URL(location.href);
    host.search = "";
    host.hash = "";
    const config = JSON.stringify({
      autoConnect: "true",
      relayHost: "127.0.0.1",
      relayPort: RELAY_PORT,
      tabId: relayTabId(),
      hostOrigin: location.origin,
      hostUrl: host.href,
      hostTitle: document.title,
    }).replace(/</g, "\\u003c");
    const html = (await response.text())
      .replace("</head>", () => `<script>window.__WEBMCP_RELAY_CONFIG=${config};</script></head>`);
    const url = URL.createObjectURL(new Blob([html], { type: "text/html" }));
    const widget = document.createElement("iframe");
    widget.src = url;
    widget.hidden = true;
    widget.setAttribute("aria-hidden", "true");
    widget.setAttribute("allow", "loopback-network; local-network; local-network-access");
    widget.addEventListener("load", () => URL.revokeObjectURL(url), { once: true });
    widget.addEventListener("error", () => URL.revokeObjectURL(url), { once: true });
    document.body.append(widget);
    this.widget = widget;
  }

  render() {
    const supported = Boolean(modelContext());
    const on = Boolean(this.controller);
    this.toggle.textContent = on ? "Turn off agent access" : "Allow agent";
    this.toggle.classList.toggle("primary", !on);
    if (!supported) this.toggle.disabled = true;
    this.renderState();
    this.renderQuestions();
    this.logList.replaceChildren(...this.log.map(({ at, name, kind, ok, text, error }) => {
      const item = document.createElement("li");
      item.classList.toggle("agent-change", kind === "change" && ok);
      item.classList.toggle("agent-user", kind === "user");
      const time = document.createElement("code");
      time.textContent = at.toLocaleTimeString();
      const tool = document.createElement("strong");
      tool.textContent = name;
      const result = document.createElement("span");
      result.textContent = !ok ? `failed: ${error}` : text ?? "read";
      item.append(time, tool, result);
      return item;
    }));
  }

  /* The parts that follow the tuner's values. The tuning and device
   * panels call this whenever they render, so the status keeps up with
   * edits by hand and disconnects. */
  renderState() {
    const supported = Boolean(modelContext());
    const on = Boolean(this.controller);
    const calls = this.log.filter(({ kind }) => kind !== "user").length;
    const changed = this.session.changed;
    this.status.textContent = !supported
      ? "This browser has no WebMCP. In Chrome, enable chrome://flags/#enable-webmcp-testing and relaunch."
      : `${on ? `Agent access on · ${calls} tool call${calls === 1 ? "" : "s"}` : "Agent access off"}${
        changed ? " · values differ from before the agent" : ""}`;
    this.status.classList.toggle("live", on);
    this.status.classList.toggle("changed", changed);
    this.revertButton.hidden = !changed;
    this.renderSnapshots();
    /* Here, so a disconnect removes cards and windows it ended. */
    this.renderRuns();
    this.renderWindows();
  }

  /* Rebuilt only when the names change, so focus stays on a button. */
  renderSnapshots() {
    const names = this.session.snapshots;
    this.snapshotList.hidden = names.length === 0;
    if (names.join("\n") !== [...this.snapshotList.querySelectorAll("[data-snapshot]")].map((button) => button.dataset.snapshot).join("\n")) {
      const label = document.createElement("span");
      label.textContent = "Snapshots";
      this.snapshotList.replaceChildren(label, ...names.map((name) => {
        const button = document.createElement("button");
        button.type = "button";
        button.dataset.snapshot = name;
        button.textContent = name;
        return button;
      }));
    }
    /* Marks the snapshot that matches the current values; not a toggle. */
    for (const button of this.snapshotList.querySelectorAll("[data-snapshot]")) {
      const current = this.session.snapshotMatches(button.dataset.snapshot);
      button.classList.toggle("current", current);
      button.setAttribute("aria-label", `Preview snapshot ${button.dataset.snapshot}${current ? " (matches the current values)" : ""}`);
    }
  }

  /* One card per open run request, rebuilt only when its status changes,
   * so focus stays on its buttons. */
  renderRuns() {
    const open = new Map(this.session.runs.open.map((run) => [run.id, run]));
    for (const card of [...this.runList.children]) {
      if (open.get(card.dataset.run)?.status !== card.dataset.status) card.remove();
    }
    const shown = new Set([...this.runList.children].map((card) => card.dataset.run));
    for (const run of open.values()) {
      if (shown.has(run.id)) continue;
      const card = document.createElement("div");
      card.className = "agent-question agent-run";
      card.dataset.run = run.id;
      card.dataset.status = run.status;
      const heading = document.createElement("strong");
      heading.textContent = "Your agent asks for a run";
      const what = document.createElement("p");
      what.textContent = describeRun(run);
      card.append(heading, what);
      if (run.note) {
        /* Quoted and attributed, so it can't pass for the tuner's text. */
        const note = document.createElement("blockquote");
        note.className = "agent-run-note";
        const from = document.createElement("strong");
        from.textContent = "Agent's note: ";
        note.append(from, run.note);
        card.append(note);
      }
      const where = run.kind === "calibration" ? "calibration panel" : "playground";
      const state = document.createElement("p");
      state.className = "muted";
      state.textContent = {
        requested: `Open the ${where} to do it, or decline. The agent only asked; nothing has changed.`,
        accepted: `Start it in the ${where}${run.kind === "calibration" ? ", then Stop when you're done" : ""}. The agent gets the result when it ends.`,
        running: `Running in the ${where}. The agent gets the result when it ends.`,
      }[run.status];
      const actions = document.createElement("div");
      actions.className = "agent-question-actions";
      const openButton = document.createElement("button");
      openButton.type = "button";
      openButton.dataset.runOpen = "";
      openButton.className = run.status === "requested" ? "primary" : "";
      openButton.textContent = run.kind === "calibration" ? "Go to calibration" : "Open playground";
      const decline = document.createElement("button");
      decline.type = "button";
      decline.dataset.runDecline = "";
      decline.textContent = run.status === "requested" ? "Decline"
        : run.status === "running" && run.kind === "calibration" ? "Stop and cancel run" : "Cancel run";
      actions.append(openButton, decline);
      card.append(state, actions);
      this.runList.append(card);
      if (run.status === "requested") this.notice("Your agent asked you for a run in Agent access.");
    }
  }

  renderWindows() {
    const recording = this.session.windows.recording;
    this.windowList.hidden = recording.length === 0;
    const shown = (this.windowList.dataset.shown ?? "").split(",");
    if (recording.map(({ id }) => id).join() === this.windowList.dataset.shown) return;
    this.windowList.dataset.shown = recording.map(({ id }) => id).join();
    for (const { id, label } of recording) {
      if (!shown.includes(id)) this.notice(`Your agent started recording telemetry “${label}”. Stop it in Agent access.`);
    }
    this.windowList.replaceChildren(...recording.map(({ id, label, maxSeconds }) => {
      const row = document.createElement("div");
      const text = document.createElement("span");
      text.textContent = `Recording telemetry “${label}” for your agent, up to ${maxSeconds} s`;
      const stop = document.createElement("button");
      stop.type = "button";
      stop.dataset.windowStop = id;
      stop.textContent = "Stop";
      stop.setAttribute("aria-label", `Stop recording “${label}”`);
      row.append(text, stop);
      return row;
    }));
  }

  /* Adds and removes cards one by one, so typing in another card isn't
   * lost. */
  renderQuestions() {
    const open = new Map([...this.session.questions.values()]
      .filter(({ status }) => status === "pending").map((entry) => [entry.id, entry]));
    for (const card of [...this.questionList.children]) {
      if (!open.has(card.dataset.question)) card.remove();
    }
    const shown = new Set([...this.questionList.children].map((card) => card.dataset.question));
    const asked = [...open.values()].filter(({ id }) => !shown.has(id));
    for (const { id, question, choices } of asked) {
      const form = document.createElement("form");
      form.className = "agent-question";
      form.dataset.question = id;
      const heading = document.createElement("strong");
      heading.textContent = "Your agent asks";
      const text = document.createElement("p");
      text.id = `agent-question-${id}`;
      text.textContent = question;
      const actions = document.createElement("div");
      actions.className = "agent-question-actions";
      for (const choice of choices) {
        const button = document.createElement("button");
        button.type = "button";
        button.dataset.choice = choice;
        button.textContent = choice;
        actions.append(button);
      }
      const input = document.createElement("input");
      input.type = "text";
      input.maxLength = 500;
      input.placeholder = "Type an answer";
      input.setAttribute("aria-labelledby", text.id);
      const send = document.createElement("button");
      send.type = "submit";
      send.className = "primary";
      send.textContent = "Send";
      const dismiss = document.createElement("button");
      dismiss.type = "button";
      dismiss.dataset.dismiss = "";
      dismiss.textContent = "Dismiss";
      actions.append(input, send, dismiss);
      form.append(heading, text, actions);
      this.questionList.append(form);
    }
    if (asked.length) this.notice("Your agent asked you a question in Agent access.");
  }
}
