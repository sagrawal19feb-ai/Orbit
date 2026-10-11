# 🔌 MCP Tool Plan for Orbit — efficient use of MCP tools

Status: **PLAN — not yet implemented** (per user request: plan first)
Target: Phase 4 (Gemini integration), split into 4a (local) + 4b (remote)

---

## 1. Why MCP

MCP (Model Context Protocol) is the open standard for connecting AI assistants to tools.
The ecosystem has **thousands of ready-made MCP servers** (Home Assistant, calendars,
search, databases, files…). If Orbit speaks MCP, it gains all of them **by voice** —
without us hand-writing every integration.

Orbit's brain is **Gemini** (function calling). MCP tools plug in through a bridge:
MCP tool schemas are JSON Schema — the *same* shape as Gemini function declarations,
so the bridge is a pass-through, not a translation layer.

## 2. Architecture — two directions, one router

```
                    ┌────────────────────────────────┐
                    │      Gemini Live API (brain)    │
                    │   function-calling interface    │
                    └───────────────┬────────────────┘
                                    │ function calls
                    ┌───────────────▼────────────────┐
                    │        MCP TOOL ROUTER          │
                    │  schema cache · allowlist ·     │
                    │  truncation · parallel calls    │
                    └───────┬────────────────┬───────┘
              local MCP │                │ remote MCP
             (in-process)│                │ (Streamable HTTP)
           ┌────────────▼─────┐   ┌────────▼─────────────┐
           │  LOCAL MCP SERVER │   │     MCP CLIENT        │
           │  device tools:    │   │  · initialize         │
           │  music · alarms · │   │  · tools/list (cache) │
           │  weather · news · │   │  · tools/call         │
           │  time · volume    │   │  · multi-server       │
           │  direct C fast-path│  │  · session reuse      │
           │  (no JSON overhead)│  │  · PSRAM buffers      │
           └──────────────────┘   └──────────────────────┘
```

## 3. Local MCP server — device tools (Phase 4a)

Every device action becomes an MCP tool with a full schema:
`play_song` · `stop_music` · `pause_music` · `resume_music` · `set_alarm` ·
`cancel_alarm` · `set_timer` · `list_alarms` · `set_volume` · `get_weather` ·
`get_news` · `get_time`

- **In-process transport**: registered with MCP schemas, but dispatched through a
  **direct C fast-path** (no JSON serialization) — zero overhead, works 100 % offline.
- One uniform tool pipeline: the Gemini bridge cannot tell local from remote tools.

## 4. Remote MCP client (Phase 4b)

- **Transport: Streamable HTTP** (MCP spec 2025-03-26): JSON-RPC 2.0 over HTTPS POST,
  `Accept: application/json, text/event-stream`, session via `Mcp-Session-Id` header.
- **Lifecycle**: `initialize` → `notifications/initialized` → `tools/list` → `tools/call`.
- **Response parsing**: handle **both** plain JSON and SSE frames (`event: message` /
  `data: {...}`) — verified: real servers answer with SSE even to POSTs.
- **Multi-server**: NVS-configured list of `{name, url, auth}`; one HTTP handle per
  server with keep-alive.
- **Verified live 2026-10-08** against public server `mcp.deepwiki.com`:
  initialize handshake OK (protocolVersion 2025-03-26, serverInfo DeepWiki 2.14.3),
  `tools/list` returned real tools with JSON-Schema `inputSchema` (`ask_wiki_question`,
  `read_wiki_contents`, …).

## 5. The bridge: MCP ↔ Gemini

| MCP | Gemini |
|---|---|
| `tools/list` → `tools[].inputSchema` (JSON Schema) | `FunctionDeclaration.parameters` — **pass-through** |
| `tools/call` params `{name, arguments}` | function call `name` + `args` |
| result `content[]` (text/image) | function response parts — **text only** (1-bit device), images dropped |
| remote tool names prefixed `server__tool` | avoids collisions across servers |

- While a tool runs, Orbit can speak an interim line ("Let me check that…") —
  Gemini Live supports text output during processing. Nice UX, cheap to add.

## 6. Efficiency — the point of this plan

1. **Schema cache in PSRAM**: `tools/list` fetched once per server (TTL 1 h, or on
   demand) — **never** re-listed per conversation turn.
2. **Lazy connect**: a remote server is contacted only when one of its tools is
   actually called (or at boot if configured eager); idle sessions closed.
3. **Context budget**: keep a per-conversation tool allowlist; if total tools exceed
   ~25, switch to a `search_tools` **meta-tool** (Anthropic's tool-search pattern) —
   dynamic discovery instead of dumping every schema into the prompt.
4. **Result truncation**: MCP results capped (~2 KB text) with a `[truncated]` marker —
   a fetched web page must never flood the audio context.
5. **Parallel calls**: independent tool calls in one turn run concurrently (FreeRTOS
   tasks, max 4 at once) — e.g. weather + news in a single question.
6. **HTTP reuse**: one `esp_http_client` handle per server, keep-alive connections.
7. **Timeouts**: 12 s per remote call + 1 retry; local calls are instant.
8. **Graceful degradation**: unreachable server → Gemini receives "tool X is unavailable
   right now" → the conversation continues; local tools are unaffected.
9. **PSRAM everywhere**: schemas, JSON-RPC payloads and results allocated with
   `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`.
10. **Offline-first**: every built-in tool works with zero network.

## 7. Configuration & UX

- **NVS** (`mcp.servers`): list of `{name, url, auth_header, token_ref}` — tokens stored
  in NVS, **never hard-coded**; URLs HTTPS-only (cert bundle).
- Config via serial console / web config (Phase 7): `orbitctl mcp add <name> <url> [token]`
- Voice UX: after adding a server, Orbit announces *"I found 12 new tools from Home
  Assistant."* — and answers *"what can you do?"* by listing its capabilities.
- **Safety**: per-server trust flag; destructive tools (e.g. HA switches) ask for a
  spoken "yes" the first time.

## 8. Phasing

| Step | What | Risk |
|---|---|---|
| **4a** | Local MCP server + router + Gemini bridge — all device tools through the uniform MCP pipeline | none (no network) |
| **4b** | Remote MCP client + NVS config + first live tests (DeepWiki / fetch-search MCP, then the user's own servers e.g. Home Assistant) | network — mitigated by timeouts + graceful degradation |
| Phase 7 | `search_tools` meta-tool, cache UX, config OTA | — |

## 9. Example: "turn off the living room light"

1. You speak → Gemini Live streams the audio.
2. Gemini emits function call `ha__turn_off_light(entity_id="light.living_room")`.
3. Router → MCP client → `tools/call` POST to your Home Assistant MCP server → light off.
4. MCP result → Gemini → Orbit speaks: *"Done — the living room light is off."*
5. Display: `THINKING` → `STANDBY`.

## 10. Deliverables

- `components/mcp_local` — in-process MCP server wrapping device tools (4a)
- `components/mcp_client` — Streamable HTTP MCP client (4b)
- `components/mcp_router` — schema cache, allowlist, truncation, parallel dispatch,
  MCP↔Gemini bridge (4a/4b)
- Config commands + NVS storage for MCP servers (4b/7)
