/* Lets the user hand the tuner's agent tools to an AI agent (#115). Allowing
 * access registers the tools with the browser's WebMCP model context and
 * opens the local relay's widget, which forwards them to a coding agent
 * running @mcp-b/webmcp-local-relay on this computer. Turning access off
 * unregisters the tools and closes the widget.
 *
 * The widget is the relay's own, vendored and pinned. It is loaded here
 * rather than with the relay's embed.js, which reloads the page when the
 * relay asks; a reload would drop the keyboard connection. */

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
  constructor(root, { tools, notice }) {
    this.tools = tools;
    this.notice = notice;
    this.log = [];
    this.toggle = root.querySelector("[data-agent-toggle]");
    this.status = root.querySelector("[data-agent-status]");
    this.logList = root.querySelector("[data-agent-log]");
    this.config = root.querySelector("[data-agent-config]");
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
      for (const tool of this.tools) {
        await context.registerTool({ ...tool, execute: (input) => this.call(tool, input) },
          { signal: controller.signal });
      }
      await this.openRelay();
      this.notice("Agent access is on. The tools only read; they can't change the keyboard.");
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
    this.render();
  }

  async call(tool, input) {
    const entry = { at: new Date(), name: tool.name, ok: true };
    this.log.unshift(entry);
    this.log.length = Math.min(this.log.length, LOG_LIMIT);
    try {
      return await tool.execute(input ?? {});
    } catch (error) {
      entry.ok = false;
      entry.error = String(error?.message ?? error);
      throw error;
    } finally {
      this.render();
    }
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
    const calls = this.log.length;
    this.status.textContent = !supported
      ? "This browser has no WebMCP. In Chrome, enable chrome://flags/#enable-webmcp-testing and relaunch."
      : on
        ? `Agent access on · ${calls} tool call${calls === 1 ? "" : "s"}`
        : "Agent access off";
    this.status.classList.toggle("live", on);
    this.logList.replaceChildren(...this.log.map(({ at, name, ok, error }) => {
      const item = document.createElement("li");
      const time = document.createElement("code");
      time.textContent = at.toLocaleTimeString();
      const tool = document.createElement("strong");
      tool.textContent = name;
      const result = document.createElement("span");
      result.textContent = ok ? "read" : `failed: ${error}`;
      item.append(time, tool, result);
      return item;
    }));
  }
}
