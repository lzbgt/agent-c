# Native tools and extensions

Date: 2026-11-13

This document owns native host-tool behavior, result/capture limits, web evidence
and extension contracts shared by the CLI and daemon. [Local web integration](LOCAL_MODEL_WEB.md)
owns browser workflow and adapter configuration; the [documentation index](README.md)
routes deployment and historical evidence. Update tool contracts here rather than
copying them into local-model runbooks.

The extension sections consolidate the former `TOOL_SERVERS.md` and
`TOOL_PLUGINS.md` material.

## Overview

There are two extension paths:

1) Tool plugins (in-process, shared library ABI)
2) Tool servers (out-of-process, JSON-lines over stdio)

Use plugins when you need low-latency calls and can trust the plugin process.
Use tool servers when you want isolation, independent dependencies, or a
separate runtime (Playwright, device bridges, AVM runners, etc.).

Platform support:
- Tool plugins: Linux, macOS, Windows
- Tool servers: Linux and macOS only (POSIX process + poll)

## Built-in host toolset

These tools are available when `--tools host` is enabled (subject to `yolo` and `host_policy`):

- `shell_exec` (runs `/bin/sh -lc <cmd>`, returns JSON envelope with `exit_code`, `timed_out`, `truncated`, `output`)
- `proc_exec` (runs an argv array via `posix_spawnp`, no shell; returns JSON envelope with `argv`, `exit_code`, `timed_out`, `truncated`, `output`)
- `file_apply_patch` (applies a unified diff via `git apply`; returns the patch as a diff-style audit trail)
- `fs_stat` (file/dir metadata; returns structured fields + a human-readable `output`)
- `fs_list` (bounded directory listing; returns structured `entries` + `output`)
- `fs_find` (bounded file discovery; returns structured `entries` + `output`)
- `fs_read` (bounded file read with pagination by line; returns `content`/`output` + `has_more` + `next_start_line`)
- `text_search` (token-safe substring search; returns structured `matches` + `output`)
- `web_search` / `web_fetch` (bounded web discovery and page evidence; see below)
- `artifact_register` (registers a retained file for client access)
- `ui_action` (typed UI-side action requests; see `docs/CLIENT.md`)

Host policy notes:
- In `--host-policy readonly`, the registry omits `shell_exec`, `proc_exec`, and `file_apply_patch`.
- In daemon restricted mode (`yolo=false`), the registry omits `shell_exec` and `proc_exec` even when `host_policy=full`.

### Cancellation and captured output

Native async job cancellation interrupts model HTTP transfers, including waits
for headers/tokens, and provider retry delays. The tool loop records a terminal
`cancelled` outcome. Browser Stop, retry and reload behavior is documented in
[Local web integration](LOCAL_MODEL_WEB.md#stop-reload-and-exact-output).

The latest run's **Tool outputs** disclosure exposes captured results during
generation. Copy/download preserve the returned tool-result text; the UI marks
truncation and shows native captured/observed byte counts. If the display itself
is clipped, these controls contain only the returned excerpt. For literal
command output, run the command with `shell_exec` or `proc_exec`. For `fs_list`,
copy/download preserves its rendered native directory listing, not Unix stdout
or structured `entries` JSON; filters and entry limits still apply. Original
captured output fulfills an exact-output request. Point to **Tool outputs** or a
registered capture file;
do not generate a large transcription or invent missing lines.

`shell_exec` and `proc_exec` accept integer `max_output_bytes` in
1..1048576, default 65536. They keep draining pipes after the retention cap.
Text captures from `shell_exec`, `proc_exec` and `fs_list` larger than 8 KiB
and at most 1 MiB are saved to a unique session-relative
`out/tool-output-XXXXXX` file with mode 0600. For `fs_list`, the saved bytes are
the rendered native listing. On successful save, model context receives a
receipt containing capture metadata and an `fs_read` path; `data.output` is
omitted entirely. For `fs_list`, `data.entries` is also omitted while entry
count, filters, entry limit and completeness remain. `output_omitted_from_prompt`,
`entries_omitted_from_prompt` (listings) and `prompt_truncated` mark these
prompt-only omissions. Read particular lines only when analysis needs them,
not to reconstruct or reprint the capture. Original tool results, UI evidence
and file bytes remain unchanged. The separate UI event summary uses a 64 KiB
prefix plus an ellipsis when clipped, flagged by
`output_display_truncated`/`output_truncated`.

The file holds all captured bytes. A capture may omit process output or
directory entries because of native limits and filters. Inspect
`captured_output_bytes`,
`observed_output_bytes`, `capture_truncated` and `capture_complete`. For
`fs_list`, `entries_count` counts returned entries under the active filters
and `max_entries`; `listing_complete` is false on entry truncation or a
traversal error. The shared writer rejects captures over 1 MiB; this and other
save failures expose `output_artifact_error` without a fabricated artifact
path. `output_artifact` is saved-file metadata and does not register an
artifact. The model can call `artifact_register` for that path.
For complete output beyond the capture cap, redirect the command to a file.

## Native web discovery and page reading

`web_search` and `web_fetch` run in the existing native host toolset and remain
available under `--host-policy readonly`. Agent C owns the tool loop; host
process execution retains its separate policy.

```json
{"query":"technology news","time_range":"day","max_results":5,"fresh":true}
{"query":"Arch Linux installation guide","engine":"google","max_results":5}
{"url":"https://wiki.archlinux.org/title/Installation_guide","mode":"auto","max_chars":12000,"fresh":true}
```

Default search requires `AGENT_SEARXNG_URL` on the daemon: an optional HTTP(S)
SearXNG origin with a literal loopback IP and explicit port; credentials, paths,
queries and fragments are rejected. Enable JSON in its `search.formats`; see the
[SearXNG search API](https://docs.searxng.org/dev/search_api.html). The native client connects directly to `/search?format=json`
without inherited proxy settings, preserves ranking and engine failures, and
distinguishes useful partial results from genuine empty results. Missing
configuration is an error. There is no automatic fallback after a configured
backend failure, partial result or empty result. SearXNG still queries external
engines, so their availability and limits apply. Keyless engines require no paid
search API credential; this is not an independent web index.

`time_range` accepts `day`, `month` or `year` for local SearXNG only; omit it
for unrestricted discovery. Combining it with `engine` is an error. It is
forwarded to SearXNG and included in the cache key. Engine support varies:
`time_range_scope: "upstream_hint"` is not a verified age guarantee.

An explicit `engine` selects ordinary browser discovery through `google`,
`bing`, `yahoo`, `mojeek` or `baidu`. The owner configures
`AGENT_BROWSER_EXECUTABLE` and `AGENT_BROWSER_PROXY`; the latter also controls
HTTP page reads. Empty proxy means direct; inline proxy credentials are
rejected. Search recognizes visible result cards and reports unsupported pages
or access gates. It never solves challenges.

Fetch modes are `auto` (default), `http` and `browser`. Auto reads bounded
HTML/plain text over HTTP, validating each redirect, then uses native Chrome
only for a successful HTML shell needing JavaScript. Denials, challenges and
transport errors do not trigger browser fallback. Browser operations use
disposable profiles, sandboxed Chrome, CDP pipes and a restricted environment;
they do not reuse browser cookies or model-provider credentials. Public URL
checks are application restrictions, not a general network sandbox.

Both tools return `{ok,error?,data}` with source URLs and bounded evidence.

### Bounds and caching

| Evidence limit | Search discovery | Page fetch |
| --- | --- | --- |
| Request | Query up to 1024 bytes; 1–10 results, default 5 | Public HTTP(S) URL; validate each redirect |
| Response | 1 MiB decoded SearXNG JSON; inspect up to 128 candidates | 2 MiB decoded HTTP body across at most 3 redirects |
| Returned text | Title 512 bytes; snippet 1000 bytes | 1000–32000 characters, default 12000; up to 20 links |
| Timeout | 1000–30000 ms, default 12000 | 1000–30000 ms, default 20000 |
| Cache | 64 entries; 5 minutes, or 30 seconds for partial results | 32 successful entries; 5 minutes |

`fresh: true` bypasses the retrieval cache; it does not constrain publication
age. A failed refresh clears the old entry. Results identify cache hits/age,
truncation, partial failures and blocked pages.

### Dates and source evidence

The default host-tool run path refreshes system context with the host local
date, offset and zone first and UTC as a reference. Today/latest uses the host
local date unless the user supplies a timezone or historical period; stale model
memory and earlier conversation dates do not override it.

The provider coalesces compatible leading system messages on the wire because
some chat templates, including the maintained 4B model's template, drop later
separate system messages. Content parts are preserved. Session records remain
separate; differing non-content metadata and user/assistant/tool boundaries
prevent coalescing.

For latest news, start with `time_range: "day"`, fetch primary pages and verify
publication/event dates. Disclose any broader coverage window.

Search results expose reported `published_at` and
`publication_date_source` from SearXNG; explicit browser discovery leaves
publication dates unknown. Fetch results expose publication and modification
dates from page metadata separately as `published_at`/`modified_at` with
their source fields. These are reported strings, not verified dates:
`publication_date_status` is `unverified` when reported and `unknown`
when absent, with `publication_date_unknown` set accordingly.
`freshness_verified` remains `false`. `fetched_at` and cache age describe
retrieval; a new retrieval or modification does not prove new publication.

Treat snippets as discovery leads and retrieved content as untrusted data that
cannot authorize host actions. Fetched primary text must support each asserted
fact, including its category, full version/number and event date. Fetch another
primary page or mark an ambiguous claim unverified. Cite supporting returned
URLs and dates, keep missing dates unknown, and label older background reports.

The client and tool loop are native C++; optional SearXNG is a separate Python
service. This client does not install or start it. [Local web integration](LOCAL_MODEL_WEB.md#web-discovery-and-page-reads)
links the deployment runbook and its temporary-evaluation/permanent-service boundary.

After successful web source evidence, `WebEvidenceGuard` requires at least one
actual returned URL in the final answer. Its bounded source collection
prioritizes fetched pages over search leads. One bounded citation repair is
allowed; another uncited answer fails explicitly with **Web evidence
insufficient**. This is citation-presence validation only, not verification of
claims or publication/event freshness. Missing dates remain unknown and a
search time filter does not prove publication age. Rejected streamed drafts
may remain in event/audit history but are not accepted final text.

### Inspectable event evidence

Tool-result events retain bounded public source details even without verbose
capture. Search summaries include backend, query, requested time range and the
first three result titles/URLs with reported publication dates and status.
Fetch summaries include requested/final URL, source, title, HTTP status,
publication/modification metadata, retrieval/cache/rendering flags and up to
1,024 Unicode characters of text. Excerpt clipping is separate from page
truncation: a summary may be clipped while the model received a complete page.

Serialized web summaries are bounded to 16 KiB. Source URLs remain whole or are
omitted with truncation flagged; excerpt clipping never invents a shorter URL.
Raw HTML, request headers and full search snippets are excluded. A successful
tool event supports source review but does not verify the model's interpretation.

HTTP extraction retains primary article text inside `aside` elements while
excluding hidden content and the existing navigation/footer filters. The
featured-table fixture checks preservation of category labels, full versions
and release dates.

Focused fixtures are `web_search_tests`, `web_discovery_tests`,
`web_fetch_tests`, `browser_process_tests` and `host_toolset_tests`. They cover
schemas, bounds, cache behavior, failures, cancellation and native registry
policy; live reachability and relevance require separate acceptance evidence.

## Token-safety guidance (filesystem tools)

- Prefer `fs_list` / `fs_read` / `fs_stat` for bounded inspection and pagination.
  A request for literal command output requires the command itself; do not
  reconstruct missing entries or substitute a truncated listing.
- Prefer `fs_find` over `find`/`tree` when you need predictable output size and default excludes.
- Prefer `text_search` over `grep -R` when you need predictable output size (bounded matches + per-file size limits).
  - Use `extensions` (e.g. `[".cpp",".h"]`) to restrict scanning.
- `fs_list`, `fs_find`, and `text_search` support `exclude_globs` (fnmatch) to filter noisy paths.
- `fs_list`, `fs_find`, and `text_search` support `respect_gitignore: true` (best-effort; reads repo-root `.gitignore`).
- `fs_stat` supports an optional bounded line count (`count_lines: true`) for small text files.
- `fs_read` supports paging (`start_line`, `max_lines`, `end_line`) and a character cap (`max_chars`).

## Tool servers (out-of-process)

Enable one or more tool servers:

./build/agentd --tools basic \
  --tool-server-cmd "python3 -u ./tests/tool_server_echo.py" \
  --tool-server-timeout-ms 30000 \
  --tool-server-max-line-bytes $((4*1024*1024)) \
  --tool-server-ping-interval-ms 0

Broker runtime member tool server example (updates a team run's runtime members):

```bash
export BROKER_BASE_URL="https://broker.example.invalid"
export BROKER_AUTH_TOKEN="...bearer..."

./build/agentd --tools basic \
  --tool-server-cmd "python3 -u ./tools/tool_server_broker_runtime_members.py"
```

Tool call shape:

```json
{
  "team_id": "team-alpha",
  "team_run_id": "run-123",
  "mode": "merge",
  "runtime_members": [
    {"member_id":"rt-1","agent_id":"agent-a","role":"executor"}
  ]
}
```

ESP32 serial/MQTT bridge reference:

```bash
./build/agentd --tools basic \
  --tool-server-cmd "python3 -u ./tools/tool_server_esp32_bridge.py --manifest ./node_caps.json --transport dry-run"
```

The bridge reads a UM-ACDS manifest (`spec_version:"um-acds/0.1"`) and exposes
each `tools[].parameters_schema` entry as an agentd tool with a provider-safe
`esp32_...` name. The original dotted UM-ACDS tool name is preserved in bridge
metadata and in the UM-BMP `TASK_ASSIGN.body.tool` / `payload.tool` fields sent
to the target node. `--transport dry-run` returns the generated envelope for
verification; `--transport serial` writes/reads newline-delimited envelopes
through `pyserial`; `--transport mqtt` uses `paho-mqtt` with request/response
topics (default `agentd/edge/{node_id}/in` and `agentd/edge/{node_id}/out`).
The serial/MQTT transports fail closed when their optional Python dependency or
transport configuration is missing.

Notes:
- tool-server flags are per-server and apply to the most recently declared
  --tool-server-cmd
- max_line_bytes bounds both the JSON response line and buffered partial lines
- optional ping (op:"ping") can be enabled to detect dead servers
- child tool-server exec now closes inherited non-stdio fds before `exec`, so
  an orphaned helper cannot accidentally keep agentd listener sockets open
  across an ungraceful daemon restart

### Protocol (JSON-lines)

All messages are single-line JSON objects terminated by \n.

Manifest request:

{"id":1,"op":"manifest"}

Manifest response:

{"id":1,"ok":true,"tools":[{"name":"server_echo","description":"...","parameters":{"type":"object","properties":{"text":{"type":"string"}},"required":["text"]}}]}

Execute request:

{"id":2,"op":"execute","tool_name":"server_echo","arguments":{"text":"hello"}}

Execute response:

{"id":2,"ok":true,"tool_result":{"ok":true,"data":{"echo":"hello"}}}

Protocol hardening:
- response id must match request id
- malformed JSON or oversized lines are treated as protocol violations
- stderr is reserved for logs (stdout is JSON-lines only)

## Sandbox mount validation

Operators can validate whether a host path may be mounted into a sandboxed tool runner
via the mount allowlist:

```bash
curl -sS -X POST "http://127.0.0.1:60306/api/v1/sandbox/mount_validate" \
  -H "Content-Type: application/json" \
  -d '{"host_path":"/Users/you/Documents","container_path":"/workspace/extra/docs","container_prefix":"/workspace/extra","is_main":true}'
```

This endpoint does not execute a sandbox; it returns an allow/deny decision and
whether the mount must be read-only.

The same allowlist is enforced for AVM capsule execution when `capsule.mounts`
is present on `/api/v1/avm/capsule_run` or an `avm_capsule` workflow task.
Validated mounts are forwarded to AVM-aware runners through
`AGENTD_AVM_MOUNTS_JSON` and `AGENTD_AVM_MOUNT_<n>_*` env vars; if the runner
ignores those vars, no host mount is exposed.

### Deterministic testing

ctest includes:
- `agentd_tool_server_smoke` for registration + basic tool execution
- `agentd_tool_server_ping_smoke` for idle ping health checks
- `agentd_tool_server_restart_smoke` for both restart-with-backoff after child
  death and same-port daemon restart after an orphaned tool-server child
- `tool_server_esp32_bridge_smoke` for UM-ACDS manifest exposure and UM-BMP
  `TASK_ASSIGN` envelope generation through the dry-run bridge transport
- `agentd_tool_server_esp32_bridge_smoke` for provider-safe tool-name exposure
  through `/api/v1/tools` and a full `/api/v1/run` tool-call round trip

## Tool plugins (in-process ABI)

The tool plugin ABI allows `agentd` to load shared libraries at runtime.

Enable:
- agentd --tool-plugin /path/to/plugin.so (repeatable)
- agentd --tool-plugin-config '{"key":"value"}' (applies to most recent plugin)

### Required symbols

Manifest:

const char* agentd_tool_plugin_manifest_json(void);

Optional config-aware manifest:

const char* agentd_tool_plugin_manifest_json_ex(const char* config_json);

Execution:

char* agentd_tool_plugin_execute_json(const char* tool_name, const char* arguments_json);

Optional config-aware execution:

char* agentd_tool_plugin_execute_json_ex(const char* tool_name, const char* arguments_json, const char* config_json);

Free:

void agentd_tool_plugin_free(char* p);

### Manifest shapes

Accepted manifest shapes:
- {"ok":true,"tools":[ ... ]}
- [ ... ] (raw array)

Each tool entry:

{
  "name": "my_tool",
  "description": "optional",
  "parameters_json": "{\"type\":\"object\",...}"
}

You may also provide parameters as a JSON object under "parameters".

### Execution return shape

Recommended:
- {"ok": true, "result": { ... }}
- {"ok": false, "error": "reason"}

### Size limits

- manifest capped at 1 MiB
- tool result capped at 4 MiB

## Embedded/MCU tool plugins (compile-time)

Embedded targets typically cannot load shared libraries. For `agent_core` builds, use a
compile-time plugin list that registers tool schemas into `agent_tool_registry_t` and
dispatches via `agent_tool_executor_t`. See:

- `docs/spec/tool_plugins_embedded_v0.md` (implemented rolling ABI + constraints)

## Sandboxed execution (plugin host)

Use the tool server protocol to run plugins out-of-process:

./build/agentd --tools basic \
  --tool-server-cmd "./build/agentd_tool_plugin_host --plugin ./build/libagentd_tool_plugin_echo.(so|dylib|dll) --plugin-config '{\"policy\":{\"limits\":{\"wall_ms\":60000,\"cpu_ms\":60000}}}'"

This provides process isolation plus tool-server timeouts and line limits.

Current proof points:
- `tests/agentd_tool_plugin_smoke.sh`
- `tests/agentd_tool_plugin_server_smoke.sh`
- `tests/agentd_tool_plugin_big_smoke.sh`
- `tests/agentd_tool_plugin_server_big_smoke.sh`
- `tests/test_tool_plugin_host_limits.cpp` (Windows host-limit validation surface)
- `tools/verify_windows_build.ps1` (Windows build/test helper for plugin loader path)

## Builtin voice media provider inspection

The experimental builtin WebRTC native-provider seam is a separate C ABI from the
tool-plugin ABI above. Operators can inspect a candidate builtin voice media
provider shared library without starting `agentd`:

```bash
python3 tools/inspect_voice_media_provider.py \
  ./build/libagentd_voice_builtin_media_engine_sample.dylib --pretty
```

The tool validates the exported provider symbol (`agentd_voice_media_engine_get_api_v2`
preferred, `..._v1` as compatibility fallback), reports ABI version and required
callback presence, and prints provider metadata such as `name`, `version`, and
declared `capabilities`.

## Native voice media stack readiness

Before attempting a real embedded WebRTC/SRTP backend, inspect the local build
machine for the required native dependencies:

```bash
python3 tools/check_voice_native_media_stack.py --pretty
```

The report checks the local `pkg-config` surface plus Homebrew formula
availability for the current candidate dependency set:

- `opus`
- `portaudio`
- `srtp`
- `libusrsctp`
- `libjuice`
- `libdatachannel`

It also summarizes whether the machine is currently ready for the narrower
`libjuice + srtp + libusrsctp + opus + portaudio` candidate path without
guessing from memory. The checker now also uses filesystem probing for cases
where package metadata is incomplete, such as `libjuice` lacking a local
`pkg-config` file and Homebrew `usrsctp` publishing an incorrect include path in
`usrsctp.pc`.

## References

- tests/tool_server_echo.py (example tool server)
- tools/agentd_tool_plugin_echo.c (example plugin)
- docs/spec/tool_plugins_sandbox_v0.md (sandbox policy details)

## Durable work state

SQLite-enabled host builds expose `work_state` for requirement updates and scoped human-action blockers. A successful call adopts that ledger for the current run's completion guard. Read-only policy permits reads and rejects mutations. The tool records work without waiting for a person; independent requirements remain runnable. See [AUTONOMOUS_WORK.md](AUTONOMOUS_WORK.md) for operations, storage paths, bounds, evidence and continuation semantics.
