# Agent tools

The web tuner can hand a set of tools to your own AI agent, such as Claude
Code, Amp, Codex or Cursor. With them the agent reads the connected keyboard's
settings, devices, telemetry and playground results directly, instead of you
exporting files and pasting them into a chat. This is the first step of #115.

The tools only read. They can't preview settings, change telemetry levels,
flash, or save anything to the keyboard.

## How it connects

The tuner page registers its tools with the browser through
[WebMCP](https://developer.chrome.com/docs/ai/webmcp). A coding agent reaches
them through [`@mcp-b/webmcp-local-relay`](https://docs.mcp-b.ai/packages/webmcp-local-relay/reference),
a small MCP server that the agent starts on your computer. When you allow
access, the tuner opens the relay's widget in a hidden frame, and the widget
forwards the page's tools to the relay over `127.0.0.1`.

```text
agent ── stdio ──► relay (127.0.0.1:9333) ◄── WebSocket ── tuner tab ── USB ──► keyboard
```

The tuner tab must be the one connected to the keyboard, and the agent must run
on the same computer as that tab. Nothing is hosted anywhere else.

## Set up

1. Use Chrome 155 or newer. Until WebMCP ships by default, enable
   `chrome://flags/#enable-webmcp-testing` and relaunch. Without it the
   **Allow agent** button stays disabled.
2. Add the relay to your agent's MCP servers. The tuner's **Agent access**
   panel shows this config with your tuner's own origin and a copy button:

   ```json
   {
     "mcpServers": {
       "zmk-tuner": {
         "command": "npx",
         "args": ["-y", "@mcp-b/webmcp-local-relay@6.0.3",
                  "--widget-origin", "https://measlesbagel.github.io"]
       }
     }
   }
   ```

   `--widget-origin` makes the relay accept tools only from that origin. Keep
   it. Note that an origin covers every page on it: `https://measlesbagel.github.io`
   also admits that account's other GitHub Pages projects. The version is
   pinned to match the tuner's copy of the relay widget.
3. Open the tuner, connect the keyboard (or start the simulator), and select
   **Allow agent**. Chrome may ask once whether the site may reach devices on
   your local network; allow it so the widget can reach the relay.
4. Ask your agent to call `get_session`.

Select **Turn off agent access** to unregister the tools and close the
connection. Every call is listed in the panel while access is on.

## Tools

| Tool | Returns |
| --- | --- |
| `get_session` | Connection (keyboard, simulator or none), transport, protocol version, and counts of targets, modified previews, devices and captured telemetry |
| `list_tuning_targets` | Every tunable stage with its stable id and devicetree path, and each parameter's label, unit, range, step, compiled value, current preview, help text and devicetree property |
| `list_pointing_devices` | Each sensor's location in the split, reachability, and current, compiled and supported CPI |
| `get_recent_telemetry` | Up to 500 of the latest captured stage telemetry events, optionally for one stage, with a readable summary of each. Detail levels are set by you in **Stage decisions** |
| `get_precision_results` | Saved Small-target precision runs from the playground |
| `get_config_handoff` | The tuning profile and the devicetree snippet that **Copy config** produces |

Everything comes from the keyboard and this browser, so the tools work for any
keyboard built with this module. Put tested values into your own ZMK config
repository: match nodes by stable id against your keymap before committing.

## Safety

- The tools are read-only and marked so (`readOnlyHint`). Run labels in
  `get_precision_results` are text you typed and are marked as untrusted
  content.
- The relay binds to loopback only.
- The relay adds a few tools of its own. One, `webmcp_open_page`, can open a
  web page in your default browser. Its request to reload the tuner is ignored
  on purpose, so it can't drop the keyboard connection.
- The relay forwards every WebMCP tool the tuner page registers. Today those
  are the six above.
- The widget is a pinned copy of the relay's own (`web/vendor/webmcp-local-relay`,
  MIT). The tuner loads it itself instead of with the relay's `embed.js`,
  which reloads the page when the relay asks; a reload would drop the keyboard
  connection.
