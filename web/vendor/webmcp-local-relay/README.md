# Vendored relay widget

`widget.html` is the browser side of
[`@mcp-b/webmcp-local-relay`](https://www.npmjs.com/package/@mcp-b/webmcp-local-relay)
6.0.3 (`dist/browser/widget.html`), unchanged, under the MIT license in
`LICENSE`. `../../agent-bridge.js` loads it in a hidden iframe when the user
allows agent access. It forwards the page's WebMCP tools to the relay on
`127.0.0.1`.

The relay's `embed.js` isn't used, because it reloads the page when the relay
asks and a reload drops the keyboard connection.

To update, copy `dist/browser/widget.html` from the new release, set
`RELAY_VERSION` in `agent-bridge.js` to that release so the MCP config the
tuner shows matches, and update the version in `docs/agent-tools.md`.
