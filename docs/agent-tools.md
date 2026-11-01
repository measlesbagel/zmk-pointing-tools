# Agent tools

The web tuner can hand a set of tools to your own AI agent, such as Claude
Code, Amp, Codex or Cursor. With them the agent reads the connected keyboard's
settings, devices, telemetry and playground results directly, instead of you
exporting files and pasting them into a chat. It can also preview values,
compare named snapshots, ask you how a change feels, ask you for labelled test
runs, and record telemetry (#115). [agent-guide.md](agent-guide.md) is a guide
for the agent itself, covering the whole round up to a pull request in your
own config repository.

Previews are the same temporary values the tuner's **Preview** buttons set.
The agent can't save them to the keyboard, flash, or change telemetry levels,
and you can undo everything it changed with one click.

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
connection. Every call is listed in the panel, with what each change did.

## Previews, snapshots and revert

The agent's preview tools send the same requests as the tuner's own controls:
one atomic batch per target, a reset, or a device resolution preview. The
keyboard validates each value, and the panels update as usual. Each call waits
until the keyboard has answered and returns the values it accepted. With the
simulator, values change in the tuner only.

- The agent's first change sets a revert point: every value as it was just
  before. The status shows **values differ from before the agent** while the
  current values differ from it, whoever changed them.
- **Revert to pre-agent values** restores the revert point. That includes
  previews you set by hand after the agent's first change, so set those again
  afterwards if you want to keep them. Revert stays available after you turn
  access off, which leaves values as they are, and turning access on again
  keeps the same revert point. If the keyboard doesn't confirm every value,
  the revert point stays so you can try again.
- The revert point and snapshots belong to the connection. Disconnecting,
  restarting the keyboard, or switching between the simulator and a keyboard
  drops them, so values saved from one are never sent to another. The values
  on the keyboard stay as they are until it restarts.
- Snapshots the agent saves appear as buttons in the panel. Select one to
  switch to it yourself, so you can compare "A" and "B" while you use the
  keyboard. The highlighted button matches the current values. Switching
  snapshots yourself sets the revert point too if the agent hasn't yet.
- `reset_previews` leaves unsaved curve edits in the panel as drafts. The
  panel's own reset buttons still discard them.
- Questions from `ask_user` appear in the panel with any answer choices the
  agent offered and a text field. You can dismiss one instead of answering.
  At most three wait at once. Turning access off cancels open questions.

## Runs and telemetry windows

- `request_run` asks you for a labelled run: a Small-target precision test,
  another playground activity, or a calibration. The request appears in the
  panel, with the agent's note quoted and marked as the agent's. **Open
  playground** (or **Go to calibration**) opens the panel with the activity
  and label set, and **Decline** refuses it. Nothing starts until you start it
  yourself. You can edit a precision run's label first; your own label comes
  back in the field afterwards. Once the request is open, the button becomes
  **Cancel run**, and for a calibration that's recording, **Stop and cancel
  run**. Only a run of the requested activity counts, and only one request is
  open at a time, so "run A, then run B" is two requests in turn. Ending a run
  early reports it as stopped, with what it measured so far.
- The agent can withdraw a request with `cancel_run`. Turning access off or a
  connection change (disconnecting, or switching between the simulator and a
  keyboard) cancels open requests.
- While a run is running, the agent's previews, resets and snapshot switches
  are refused, so the run measures the values it started with. You can still
  switch snapshots or change values yourself.
- Each result records the values the run started with: those that differ from
  compiled defaults, and the snapshots they match. `netChangeDuringRun` lists
  values that differ between the start and the end of the run, whoever changed
  them; a value changed and set back during the run doesn't appear.
  `compare_runs` lines up runs of the same activity metric by metric.
- `start_telemetry_window` records every stage sample that arrives until
  `stop_telemetry_window`, the time limit (2 minutes by default, at most 15)
  or 50,000 samples, separately from the timeline's last 2000 events. It
  records what you have turned on in **Stage decisions** and doesn't change
  detail levels, so it warns when every stage is off. When one starts, the
  page says so, and while it records, the panel shows it with a **Stop**
  button.
- Window summaries count events per stage and, for every-frame samples,
  arrival intervals and travel per signal kind. Raw samples come in pages of
  up to 1000. Dropped samples are the larger of the firmware's drop counter
  over the window (read again at start and stop) and the gaps in the sample
  sequence. Both cover every stage, not only the ones the window records:
  the firmware has one counter and one sequence for all stages. Over Bluetooth, every-frame telemetry drops what the link can't
  carry ([protocol](protocol.md#bandwidth)), and the summary says so.
- Disconnecting ends a recording window. Turning access off stops windows and
  forgets them with their samples.

## Tools

| Tool | Returns |
| --- | --- |
| `get_session` | Connection (keyboard, simulator or none), transport (`USB serial` or `Bluetooth`), protocol version, and counts of targets, modified previews, devices and captured telemetry |
| `list_tuning_targets` | Every tunable stage with its stable id and devicetree path, and each parameter's label, unit, range, step, compiled value, current preview, help text and devicetree property |
| `list_pointing_devices` | Each sensor's location in the split, reachability, and current, compiled and supported CPI |
| `get_recent_telemetry` | Up to 500 of the latest captured stage telemetry events, optionally for one stage, with a readable summary of each. Detail levels are set by you in **Stage decisions**. For a stretch of use, a telemetry window is better |
| `get_precision_results` | Saved Small-target precision runs from the playground |
| `get_config_handoff` | The tuning profile and the devicetree snippet that **Copy config** produces |
| `set_tuning_values` | Previews one or more parameters of one target as one batch, keyed by parameter key. Returns each parameter's previous, requested and accepted value |
| `preview_device_cpi` | Previews a sensor's resolution |
| `reset_previews` | Restores compiled defaults for one target, one device, or everything |
| `save_snapshot`, `apply_snapshot`, `list_snapshots` | Saves every target value and device resolution under a name, previews a saved snapshot (only what differs), and lists snapshots with whether each matches the current values |
| `ask_user` | Shows a question in the tuner and returns a question id at once |
| `get_user_answer` | Waits up to 45 seconds (20 by default) for the answer, then returns it or `pending` |
| `request_run` | Asks you for a labelled precision, playground or calibration run and returns a run id at once |
| `cancel_run` | Withdraws an open run request |
| `get_run_result` | Waits up to 45 seconds (20 by default) while the run is open, then returns its status (`requested`, `accepted`, `running`, `finished`, `stopped`, `declined` or `cancelled`), the values it started with, and its result. Without an id, lists runs |
| `compare_runs` | Two to six runs of the same activity side by side, with each metric's change from the first |
| `start_telemetry_window` | Starts recording stage telemetry under a label and returns a window id at once |
| `stop_telemetry_window` | Stops a window and returns its summary, with dropped samples |
| `get_telemetry_window` | A window's summary while recording or after, optionally with pages of raw samples. Without an id, lists windows |

Every result is a JSON object. A tool that fails returns an MCP error result
with the reason, such as a value outside the parameter's range.

Everything comes from the keyboard and this browser, so the tools work for any
keyboard built with this module. Put tested values into your own ZMK config
repository: match nodes by stable id against your keymap before committing, as
[agent-guide.md](agent-guide.md) describes.

## Safety

- Tools that only read are marked `readOnlyHint`. The tools that preview,
  save snapshots or ask questions are not, and `reset_previews`, which can
  drop every preview at once, is also marked `consequentialHint`. Requesting
  or withdrawing a run and starting or stopping a telemetry window aren't
  marked read-only either, since they change what the tuner shows or records;
  reading a window is. Run labels, run results, telemetry and your answers are
  marked as untrusted content.
- Nothing the agent does persists: previews last until the keyboard restarts,
  and **Revert to pre-agent values** undoes them sooner.
- The relay binds to loopback only.
- The relay adds a few tools of its own. One, `webmcp_open_page`, can open a
  web page in your default browser. Its request to reload the tuner is ignored
  on purpose, so it can't drop the keyboard connection.
- The relay forwards every WebMCP tool the tuner page registers. Today those
  are the twenty-one above.
- The widget is a pinned copy of the relay's own (`web/vendor/webmcp-local-relay`,
  MIT). The tuner loads it itself instead of with the relay's `embed.js`,
  which reloads the page when the relay asks; a reload would drop the keyboard
  connection.
