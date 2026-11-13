# Local model web integration

This guide owns the Agent C browser workflow and loopback adapter contract.
For model weights, the maintained 4B profile, launcher commands, host ports,
private key locations and operational troubleshooting, use the
[AI Model deployment runbook](https://github.com/lzbgt/tiny-multimodal-edge-ai/blob/main/docs/local-4b-agent.md)
(`/home/bruce/work/ai_model/docs/local-4b-agent.md` on the Arch host).
For native tool behavior and evidence limits, use [Tools](TOOLS.md).
The [documentation index](README.md) maps the remaining guides.

The on-demand local profile reuses the existing UI and native daemon. The Go
adapter serves built assets, authenticates the browser and forwards supported
agent APIs. Model turns, tool selection, host execution, sessions, cancellation
and streaming remain in the native runtime.

| Responsibility | Maintained source |
| --- | --- |
| Model profile and process supervision | AI Model repository: `scripts/deepseek_qwen35.py` and its manifest |
| Loopback UI server and configuration | `broker/internal/localui/` and `broker/cmd/agentd-local-ui/` |
| Browser workspace and local defaults | `ui/`, including `ui/src/hooks/useUiSettings.ts` |
| Execution and native tools | `core/`, `cli/` and `daemon/`; contracts in [Tools](TOOLS.md) |

The owner-stopped production daemon, gateway and PostgreSQL deployment are
separate. This profile does not enable those services or require their database,
OAuth application or cloud model keys. The [production hold](ARCH_HOST_DEPLOYMENT.md#owner-stop-2026-11-12)
remains effective.

## Open the workspace

Start the maintained profile using the AI Model runbook and open the ready URL
printed by its launcher. Unlock the workspace with that profile's private login
key. The browser receives an expiring HttpOnly, SameSite=Strict session cookie;
the key is not embedded in UI configuration or saved in browser storage.
Sessions expire after 24 hours or an adapter restart. **Lock workspace** clears
the browser cookie.

Choose **New chat**, then ask for a task. **Chat session** selects saved chats.
**Stop** controls the active run, and **Tool outputs** exposes captured results
directly below the chat. **Trace** and **Run comparison** retain the native
inspection views. Cloud provider, broker, workflow and administration controls
are hidden in this local profile.

Example:

> Search the live web for the Arch Linux installation guide, open the official
> result, run uname -s using the host tool, and report the actual command output
> with the source URL.

## Stop, reload and exact output

**Stop** requests native cancellation, including during model prefill, token
generation, host tools and provider retry waits. The control remains
**Stopping…** until the terminal job update arrives. A rejected request displays
**Stop failed** and permits retry. The underlying transport/tool-loop behavior
is specified in [Cancellation and captured output](TOOLS.md#cancellation-and-captured-output).

Reloading restores the active watcher from this browser and replays retained
events, so earlier tool results and Stop remain available. A dropped stream
falls back to progress polling. This state is browser-local; adapter restart or
cookie expiry may require unlocking again.

Open **Tool outputs** as soon as a result appears, without waiting for the final
answer. **Copy output** and **Download output** preserve the returned text and
whitespace even when the preview is collapsed. A flagged display excerpt
contains only the returned excerpt; these controls cannot recover omitted
bytes or automatically download a saved capture.

For `fs_list`, this text is the rendered native directory listing, with its
filters and entry limits. It is not Unix command stdout or structured `entries`
JSON. A literal command-output request requires the command itself. The
assistant should point to the original result or a registered capture file
instead of generating a large transcription. Capture limits, saved-file
receipts and model-context omissions are defined once in [Tools](TOOLS.md#cancellation-and-captured-output).

## Web discovery and page reads

Configure discovery through the AI Model runbook. The adapter forwards agent
requests; the native host toolset selects and executes `web_search` and
`web_fetch`. Default discovery uses an explicitly configured local SearXNG
service; an explicit `engine` selects isolated browser discovery. The launcher
does not start SearXNG. Its optional Python service is a separate deployment
dependency, and temporary evaluation does not enable a permanent service.

Use [Native web discovery and page reading](TOOLS.md#native-web-discovery-and-page-reading)
for schemas, backend selection, HTTP/browser fallback, proxy behavior, evidence
bounds, cache lifetimes and failures. For current-news tasks, ask for the
required date window and fetched primary sources. The native date and citation
rules are in [Dates and source evidence](TOOLS.md#dates-and-source-evidence):
retrieval time is not publication time, and a returned citation does not itself
verify a claim.

## Build and start the adapter

Configure the native build through [Operations](OPERATIONS.md), then build the
existing components:

```sh
cmake --build build-snapshot --target agent agentd -j 4
(cd broker && go test -race ./internal/localui ./cmd/agentd-local-ui)
(cd broker && go build -trimpath -o ../build-snapshot/agentd-local-ui ./cmd/agentd-local-ui)
npm run build --prefix ui
```

The maintained launcher supplies profile values to the adapter. For direct
integration against an already running local daemon, the adapter accepts:

```sh
build-snapshot/agentd-local-ui \
  --listen 127.0.0.1:18088 \
  --daemon-url http://127.0.0.1:18090 \
  --daemon-token-file /path/to/private/daemon-key \
  --login-key-file /path/to/private/api-key \
  --web-dir "$PWD/ui/dist" \
  --model local-model-id \
  --model-base-url http://127.0.0.1:18089/v1
```

Replace the example identity and paths with the selected profile. All key files
must be private regular files (mode 0600). The daemon token is distinct from the
browser/model key. The daemon receives its model key privately; the adapter
strips browser `api_key` fields and supplies only its private daemon bearer
credential upstream.

## Request contract and limits

Both upstream URLs and the listener require explicit loopback IP addresses.
The model URL must use HTTP and cannot contain credentials, query or fragment.
The adapter disables proxy discovery for its agentd connection and rejects
public listeners or external model endpoints at startup.

HTTP Host and request Origin are checked. The adapter advertises no CORS access
and rejects cross-origin API requests; a browser mutation requires an exact
same-origin request. Authorized local API clients can use the private login key
as a bearer credential without a cookie. Model/daemon keys never appear in
`/agentui-config.js`.

For `POST /api/v1/run` and `POST /api/v1/run_async`, the adapter pins the configured
model/base URL, an empty inference proxy, native host tools, full host policy
and the full automation profile. Its configured limits also initialize the
browser defaults, overriding stale browser-local values. When the AI Model
launcher is used, its manifest owns the supplied profile values.

| Adapter flag | Standalone default | Meaning |
| --- | --- | --- |
| `--max-output-tokens` | 8192 | Provider response budget |
| `--max-chars` | 480000 | UTF-8 conversation-compaction threshold |
| `--timeout-ms` | 600000 | Run request timeout |
| `--max-steps` | 32 | Model steps |
| `--max-tool-calls-total` | 64 | Total tool calls |
| `--max-repeated-tool-calls` | 3 | Repeated-call limit |
| `--max-tool-call-args-chars` | 65536 | Tool argument character limit |
| `--max-tool-result-chars` | 24000 | Tool-result budget delivered to model context |

The compaction threshold is not an exact token window. The model server enforces
the actual configured context; tool capture and UI display limits are separate
[output layers](TOOLS.md#cancellation-and-captured-output).

Run JSON is limited to 1 MiB, login JSON to 8 KiB, and other supported request
bodies to 32 MiB, subject also to daemon upload limits. SSE responses flush
immediately and propagate client disconnection upstream.

The API allowlist covers chat runs, jobs/stream/cancel, session creation/history,
uploads/deletion, client events, tool listing, local configuration reads,
trace/run comparison and artifact reads. Configuration mutation, provider tests,
cloud model catalogs, orchestration, workflows and edge operations are not
exposed. The daemon remains a private upstream with its own credential.

Raw file/blob responses use a sandbox policy without script or same-origin
permissions and disable MIME sniffing. Only explicit raster image, plain text,
audio and video MIME types retain previews; HTML, SVG, JavaScript and unknown
types become binary downloads. MIME rewriting also protects Blob URL previews.
JSON metadata and built assets retain normal types. The local profile does not
read or write server-side client preferences.

Unauthenticated `GET /healthz` checks only the adapter process. Authenticated
`GET /api/v1/health` forwards daemon health. Neither proves model readiness; the
launcher checks model, daemon and web health separately before printing its
ready URL.

## Verification and recorded evidence

The Go adapter fixtures cover login/session expiry, origin/Host policy, private
headers, fixed run parameters, denied routes, body bounds, static paths/symlinks,
loopback configuration, artifact MIME isolation and streaming cancellation.
`ui/e2e/local_job_controls.spec.ts` uses mocked APIs for reload/replay, Stop
rejection/pending state and exact output copy/download. Native fixtures are
listed with their owning contracts in [Tools](TOOLS.md).

Live acceptance additionally needs real output during generation, exact
copy/download, recovery after reload, Stop reaching terminal `cancelled`,
a successful follow-up request and a dated answer supported by fetched sources.
Mocked checks do not establish model quality or live search relevance. The
[AI Model runbook](https://github.com/lzbgt/tiny-multimodal-edge-ai/blob/main/docs/local-4b-agent.md)
links dated reports and their model/runtime configuration; those results remain
evidence for the recorded run.
