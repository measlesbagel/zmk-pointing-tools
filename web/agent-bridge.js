/* Lets the user hand the tuner's agent tools to an AI agent (#115). Allowing
 * access starts an agent session, registers its tools with the browser's
 * WebMCP model context and opens the local relay's widget, which forwards
 * them to a coding agent running @mcp-b/webmcp-local-relay on this
 * computer. Turning access off unregisters the tools and closes the widget
 * but leaves values as they are: Revert agent changes stays available until
 * the next session starts.
 *
 * The widget is the relay's own, vendored and pinned. It is loaded here
 * rather than with the relay's embed.js, which reloads the page when the
 * relay asks; a reload would drop the keyboard connection. */

import { AgentSession, describeChanges } from "./agent-tools.js";

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
  constructor(root, { context, notice }) {
    this.context = context;
    this.notice = notice;
    this.log = [];
    this.session = undefined;
    this.shownQuestions = "";
    this.toggle = root.querySelector("[data-agent-toggle]");
    this.status = root.querySelector("[data-agent-status]");
    this.logList = root.querySelector("[data-agent-log]");
    this.config = root.querySelector("[data-agent-config]");
    this.revertButton = root.querySelector("[data-agent-revert]");
    this.snapshotList = root.querySelector("[data-agent-snapshots]");
    this.questionList = root.querySelector("[data-agent-questions]");
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
        this.session?.dismiss(id);
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
    this.session?.close();
    this.session = new AgentSession(this.context, () => this.render());
    try {
      for (const tool of this.session.tools) {
        await context.registerTool({ ...tool, execute: (input) => this.call(tool, input) },
          { signal: controller.signal });
      }
      await this.openRelay();
      this.notice("Agent access is on. The agent can preview settings; Revert agent changes undoes them.");
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
    this.session?.close();
    if (this.session?.changed) this.notice("Agent access is off. The agent's changes stay until you revert them.");
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
      if (entry.kind === "change") entry.text = JSON.parse(result).summary;
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
    if (!this.session) return;
    this.revertButton.disabled = true;
    try {
      const { changes, remaining } = await this.session.revert();
      this.record(`Reverted agent changes: ${describeChanges(changes)}`);
      this.notice(remaining.length
        ? `Some values didn't revert: ${describeChanges(remaining)}. Try again.`
        : "Restored the values from before the agent's first change.", remaining.length > 0);
    } catch (error) {
      this.notice(`Could not revert agent changes: ${error.message}`, true);
    } finally {
      this.revertButton.disabled = false;
      this.render();
    }
  }

  async applySnapshot(name) {
    try {
      const { changes } = await this.session.applySnapshot(name);
      this.record(`Applied snapshot "${name}": ${describeChanges(changes)}`);
    } catch (error) {
      this.notice(`Could not apply snapshot "${name}": ${error.message}`, true);
    }
  }

  answer(id, text) {
    this.session?.answer(id, text);
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
    const calls = this.log.filter(({ kind }) => kind !== "user").length;
    const changed = Boolean(this.session?.changed);
    this.status.textContent = !supported
      ? "This browser has no WebMCP. In Chrome, enable chrome://flags/#enable-webmcp-testing and relaunch."
      : `${on ? `Agent access on · ${calls} tool call${calls === 1 ? "" : "s"}` : "Agent access off"}${
        changed ? ` · agent changes ${on ? "applied" : "still applied"}` : ""}`;
    this.status.classList.toggle("live", on);
    this.status.classList.toggle("changed", changed);
    this.revertButton.hidden = !changed;
    this.renderSnapshots();
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
      result.textContent = !ok ? `failed: ${error}` : kind === "read" ? "read" : text;
      item.append(time, tool, result);
      return item;
    }));
  }

  renderSnapshots() {
    const names = [...(this.session?.snapshots.keys() ?? [])];
    this.snapshotList.hidden = names.length === 0;
    const label = document.createElement("span");
    label.textContent = "Snapshots";
    this.snapshotList.replaceChildren(label, ...names.map((name) => {
      const button = document.createElement("button");
      button.type = "button";
      button.dataset.snapshot = name;
      button.textContent = name;
      button.title = `Preview snapshot "${name}"`;
      button.setAttribute("aria-pressed", String(this.session.snapshotMatches(name)));
      return button;
    }));
  }

  /* Rebuilt only when the open questions change, so typing isn't lost. */
  renderQuestions() {
    const open = [...(this.session?.questions.values() ?? [])].filter(({ status }) => status === "pending");
    const shown = open.map(({ id }) => id).join(",");
    if (shown === this.shownQuestions) return;
    const asked = open.some(({ id }) => !this.shownQuestions.split(",").includes(id));
    this.shownQuestions = shown;
    this.questionList.replaceChildren(...open.map(({ id, question, choices }) => {
      const form = document.createElement("form");
      form.className = "agent-question";
      form.dataset.question = id;
      const heading = document.createElement("strong");
      heading.textContent = "Your agent asks";
      const text = document.createElement("p");
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
      input.setAttribute("aria-label", "Answer");
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
      return form;
    }));
    if (asked) this.notice("Your agent asked you a question in Agent access.");
  }
}
