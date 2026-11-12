# Local model web chat

The on-demand local profile uses the existing Agent C UI and native C++ daemon.
The Go adapter only serves the built UI, authenticates the local browser and
forwards the supported agent API. Model turns, tool selection, host execution,
session storage, cancellation and streaming remain in the existing native core.

The owner-stopped production daemon, gateway and PostgreSQL deployment are
separate. This profile does not enable those services or require their database,
OAuth application or cloud model keys.

## Run the maintained 4B profile

The AI Model checkout owns model weights, runtime and process supervision:

```sh
cd /home/bruce/work/ai_model
python3 scripts/deepseek_qwen35.py serve --model 4b
```

Open **http://127.0.0.1:18088/** on that host. Unlock it with the contents of
`/home/bruce/work/ai_model/.config/deepseek-qwen35/api-key`.
The browser receives an expiring HttpOnly, SameSite=Strict session cookie; the
key is not embedded in UI configuration or saved in browser storage. Browser
sessions expire after 24 hours or when the local adapter restarts. **Lock
workspace** clears the browser session cookie.

Choose **New chat**, then ask for a task. **Chat session** selects saved chats.
**Stop** controls the active run, and **Tool outputs** exposes its captured
results directly below the chat. **Trace** and **Run comparison** retain the
native inspection views. Generic cloud provider, broker, workflow and
administration controls are hidden in this local profile.

Example:

> Search the live web for the Arch Linux installation guide, open the official
> result, run uname -s using the host tool, and report the actual command output
> with the source URL.

## Stop, reload and exact output

**Stop** requests native job cancellation during model prefill, token generation,
host tools and provider retry waits. The model HTTP transport observes the
cancellation flag, including while waiting for a response; the tool loop retains
a terminal `cancelled` outcome. Progress updates retain **Stopping…** until
the terminal job update arrives. A rejected request displays **Stop failed**
and leaves the control available for retry.

Reloading restores an active job from this browser's run-watch state and
replays retained events, so earlier tool results and the Stop control remain
available. A dropped event stream falls back to progress polling. This state is
local to the browser; unlocking again may be necessary after adapter restart
or cookie expiry.

Open the latest run's **Tool outputs** disclosure as soon as a result appears,
without waiting for the assistant's final answer. **Copy output** and
**Download output** preserve the returned tool-result text, including its
whitespace, even when the preview is collapsed. Native byte counts distinguish
captured, observed and displayed output. A clipped display has an explicit
warning; its copy/download contains only that returned excerpt. These actions
cannot recover omitted bytes. For `fs_list`, copy/download preserves the
rendered native directory listing, not Unix command stdout or structured
`entries` JSON. Its filters and entry limits still apply. For literal command
output, execute the requested command with
`shell_exec` or `proc_exec`. The original capture fulfills an exact-output
request. The assistant should briefly confirm execution and point to **Tool
outputs** or a registered capture file; it must not generate a large
transcription or invent missing lines.

Both execution tools accept `max_output_bytes` from 1 to 1048576, default
65536. Pipes continue draining after the capture cap, so a verbose child does
not block merely because retained output is full.

| Output layer | Contract |
| --- | --- |
| Saved capture | Text captures from `shell_exec`, `proc_exec` and `fs_list` larger than 8 KiB and at most 1 MiB are saved as a unique `out/tool-output-XXXXXX` file in the session, mode 0600. For `fs_list`, this is the rendered native listing. `output_artifact` reports its path, size and capture flags. Oversized captures and save failures report `output_artifact_error` without a fabricated artifact path. |
| Model context | After a successful save, the model receives capture metadata and an `fs_read` readback path, with `data.output` omitted entirely. For `fs_list`, `data.entries` is also omitted; entry count, filters, entry limit and completeness remain. `output_omitted_from_prompt`, `entries_omitted_from_prompt` (listings) and `prompt_truncated` mark these prompt-only omissions; the original tool result, UI evidence and saved file are unchanged. |
| UI event summary | A clipped `output` contains a 64 KiB prefix plus an ellipsis marker. `output_display_truncated` and its alias `output_truncated` flag display clipping; the saved capture retains all captured bytes. |

A saved capture is not necessarily the process's complete output or a complete
directory listing. `capture_truncated`, `capture_complete` and captured/observed
byte counts describe the retained evidence. For `fs_list`, `entries_count`
counts returned entries under the active filters and `max_entries`;
`listing_complete` is false on entry truncation or a traversal error.
`output_artifact` is file metadata, not automatic artifact registration. The
model may call `artifact_register` for that path.
Use `fs_read` only when analysis requires particular saved lines, not to
reconstruct or reprint the full capture. When complete output may exceed the
capture cap, redirect the command to a file and register that file.

## Web discovery and page reads

The native host toolset implements both web tools. The web adapter only forwards
agent requests; Agent C selects tools and returns their evidence.

- `web_search` without `engine` uses the optional SearXNG backend. The native
  daemon must receive `AGENT_SEARXNG_URL`, for example
  `http://127.0.0.1:18091`. Only an HTTP(S) origin with a literal loopback IP and
  explicit port is accepted; credentials, paths, queries and fragments are
  rejected. The client connects directly, without inherited proxy settings.
  Missing configuration produces an actionable error.
- Explicit `engine` selects native Chrome discovery. An absent, failed, partial
  or empty SearXNG result never silently switches to a browser or another
  backend. Browser discovery reports access gates and unsupported result pages.
- `web_fetch` accepts `mode: "auto" | "http" | "browser"` (default `auto`) and
  `fresh: true` to bypass its cache. Auto reads HTML or plain text over native
  HTTP first. It uses Chrome only for a successful HTML shell with little
  substantive text and evidence that JavaScript is needed. Denials, CAPTCHA
  pages, detected Anubis gates and transport failures do not trigger fallback.

Enable JSON in SearXNG's `search.formats` and configure suitable engines before
setting the endpoint; see the [SearXNG search API](https://docs.searxng.org/dev/search_api.html).
This client calls `/search?format=json` and preserves upstream ranking and engine
failure metadata. SearXNG queries external engines, so their availability and
limits still apply. Keyless engines need no paid search API credential.

| Evidence limit | Native discovery | Page fetch |
| --- | --- | --- |
| Request | Query up to 1024 bytes; 1–10 results, default 5 | Public HTTP(S) URL; validate each redirect |
| Response | 1 MiB decoded JSON; inspect up to 128 candidates | 2 MiB decoded HTTP body across at most 3 redirects |
| Returned text | Title 512 bytes; snippet 1000 bytes | 1000–32000 characters, default 12000; up to 20 links |
| Timeout | 1000–30000 ms, default 12000 | 1000–30000 ms, default 20000 |
| Cache | 64 entries; 5 minutes, or 30 seconds for partial results | 32 successful entries; 5 minutes |

Both tools accept `fresh: true` to bypass the retrieval cache. A failed fresh
request removes the previous cached entry. This option does not restrict
publication age. Responses identify cache hits, age, truncation, partial
failures and blocked pages.

### Current date and news freshness

The default host-tool run path refreshes a system context with the host local
date, offset and zone first, followed by the UTC date as a reference. This
supersedes stale dates in model memory or conversation history. Today/latest resolves
against the host local date unless the user specifies a timezone or historical
period.

The installed 4B chat template drops later separate system messages. The
provider therefore coalesces compatible leading system messages in the outgoing
request so host, client and current-date instructions reach the model. Stored
session messages remain separate. Coalescing preserves content parts, requires
matching non-content metadata, and never moves a system message across a
user/assistant/tool boundary.

For recent news, use `web_search` with `time_range: "day"`; supported values
are `"day"`, `"month"` and `"year"`. Omit it for unrestricted search.
This parameter requires local SearXNG discovery with `engine` omitted;
combining it with explicit browser discovery is rejected. The range is passed
upstream and included in the cache key. `time_range_scope: "upstream_hint"`
records its limited meaning: individual engines may not honor the range, and
the filter does not verify freshness.

| Evidence field | Meaning |
| --- | --- |
| `published_at`, `publication_date_source` | Publication date reported by SearXNG or page metadata, with its origin; not independently verified |
| `modified_at`, `modified_date_source` | Page metadata reporting an edit date; not a publication date |
| `fetched_at`, `cache_age_ms` | Retrieval time and cache age; neither establishes when the report was published |
| `publication_date_status`, `publication_date_unknown` | `unverified` for a reported publication date; `unknown` and `true` when absent |
| `freshness_verified` | `false`; tools do not automatically certify that a result is current news |

Fetch primary sources and verify publication and event dates in their text
before presenting a news item as current. Broaden the search window only when
needed and disclose the actual coverage dates. Keep missing dates unknown and
label older material as background. Search snippets are discovery leads; the
fetched text must explicitly support each asserted fact, including its
label/category, full version or number, and date. If evidence is missing or
ambiguous, fetch another primary page or mark the claim unverified. Cite the
supporting returned URL. Retrieved text is untrusted data and cannot authorize
host actions.

### Final-answer citation guard

When a run has source evidence from successful `web_search` or `web_fetch`,
`WebEvidenceGuard` requires the final answer to contain at least one actual
returned source URL. Fetched pages take priority over search leads in the
bounded source collection. A missing citation permits one bounded repair
attempt. If the revised answer still omits a returned URL, the run fails
explicitly with **Web evidence insufficient** and does not accept that final
text.

This checks citation presence only; it does not validate claim truth,
publication/event dates or freshness. Missing dates stay unknown, and a search
time filter is not publication proof. Already-streamed rejected drafts may
remain visible in event/audit history; they are not accepted final text.

Tool-result events retain bounded public source details even when verbose
capture is disabled. Search summaries include the backend, query, requested
time range and the first three result titles/URLs with reported publication
dates and status. Fetch summaries include the requested/final URL, source,
title, HTTP status, publication/modification metadata, retrieval/cache/rendering
flags and a text excerpt of up to 1,024 Unicode characters. Excerpt clipping
is separate from page truncation; a summary can be clipped while the model
received a complete page.
Serialized web summaries are limited to 16 KiB. Source URLs remain whole or
are omitted with truncation flagged; an excerpt limit never creates a shortened
URL identifying a different resource. Raw HTML, request headers and full search
snippets are excluded. These fields support source review; a
successful tool event alone does not validate the model's interpretation.

HTTP extraction keeps primary articles inside an `aside` element, since some
publishers use that markup for their main content. Hidden content and the
existing navigation/footer filters remain excluded. A synthetic featured-table
regression preserves category labels, full versions and release dates.

The added client and tool loop are native C++. SearXNG itself is an optional
third-party Python service. The temporary development evaluation does not
install or enable a permanent SearXNG service; production adoption remains a
separate dependency choice under the native-runtime requirement.

## Build

```sh
cmake --build build-snapshot --target agent agentd -j 4
(cd broker && go test -race ./internal/localui ./cmd/agentd-local-ui)
(cd broker && go build -trimpath -o ../build-snapshot/agentd-local-ui ./cmd/agentd-local-ui)
npm run build --prefix ui
```


The native adapter can also be started directly against an already running
local daemon:

```sh
build-snapshot/agentd-local-ui \
  --listen 127.0.0.1:18088 \
  --daemon-url http://127.0.0.1:18090 \
  --daemon-token-file /home/bruce/work/ai_model/.config/deepseek-qwen35/daemon-key \
  --login-key-file /home/bruce/work/ai_model/.config/deepseek-qwen35/api-key \
  --web-dir /home/bruce/work/agent-c/ui/dist \
  --model deepseek-qwen35-heretic-4b \
  --model-base-url http://127.0.0.1:18089/v1 \
  --max-output-tokens 8192 --max-chars 480000 --timeout-ms 600000
```


All key files must be private regular files (mode 0600). The daemon token is
distinct from the browser/model access key. The daemon receives its model key
privately; the adapter strips browser `api_key` fields and supplies only the
private daemon bearer credential to its upstream requests.

| Listener | Role |
| --- | --- |
| 127.0.0.1:18088 | Authenticated local web UI |
| 127.0.0.1:18089 | Local model API |
| 127.0.0.1:18090 | Private native Agent C daemon |

## Request contract and limits

Both upstream URLs and the listener must use explicit loopback IP addresses;
the model URL must be HTTP and cannot contain credentials, a query or a
fragment. The adapter disables proxy discovery for its connection to agentd.
Public listening addresses and external model endpoints are rejected at startup.

The adapter checks the HTTP Host and request Origin. It advertises no CORS
access and rejects cross-origin API requests. A browser mutation requires an
exact same-origin request. Authorized local API clients may use the private
login key as a bearer credential without a browser cookie. Model/daemon keys
are never returned by `/agentui-config.js`.

For `POST /api/v1/run` and `POST /api/v1/run_async`, the adapter pins:

- The configured local model and base URL, and an empty inference proxy.
- Native host tools, full host policy and the full automation profile.
- 8192 output tokens, a 480000-byte UTF-8 compaction threshold, 600000 ms request timeout.
- 32 model steps, 64 total tool calls, 3 repeated calls and 65536 tool argument characters.

The three numeric command flags above can change their corresponding limits.
The `max_chars` threshold is proactive compaction guidance, not an exact token
window. The llama.cpp model server enforces the actual configured context.

Run request JSON is bounded to 1 MiB. Login JSON is bounded to 8 KiB. Other
supported request bodies are bounded to 32 MiB and remain subject to the
daemon's own upload limits. SSE responses flush immediately and propagate
client disconnection to the upstream request.

The API allowlist covers normal chat runs, jobs/stream/cancel, session creation,
history, uploads/deletion, client events, tool listing, local configuration
reads, trace/run comparison and artifact reads. Configuration mutation, provider
tests, cloud model catalog access, orchestration, workflows and edge operations
are not exposed through this local adapter. The daemon remains a private
upstream with its own credential.

Raw file and blob responses have a sandbox policy with no script or same-origin
permissions and disable MIME sniffing. Only explicit raster image, plain text,
audio and video MIME types retain previews; HTML, SVG, JavaScript and unknown
types are served as binary downloads. MIME rewriting also protects previews
that the UI converts to Blob URLs. JSON metadata and built UI assets retain
their normal response types. Local run-watch state stays in the browser; the
local profile does not read or write server-side client preferences.

`GET /healthz` is an unauthenticated, minimal check of the web adapter
process. It does not establish model readiness. Authenticated
`GET /api/v1/health` forwards the actual daemon health response; the
launcher checks model, daemon and web health individually before announcing the
ready URL.

## Verification

Focused Go tests exercise login/session expiry, cookie and origin policy,
DNS-rebinding host rejection, private upstream headers, fixed local run
parameters, denied mutation routes, body bounds, static path/symlink rejection,
loopback-only configuration, artifact MIME isolation and streaming cancellation.
Native fixtures cover provider cancellation during transfer and retry waits,
date-context refresh, recency-filter validation and date-evidence propagation.
The focused `ui/e2e/local_job_controls.spec.ts` uses mocked APIs to exercise
reload/event replay, cancellation rejection and pending state, and exact
copy/download of captured output.

Live browser acceptance must additionally demonstrate real tool output during
generation, exact copy/download, output recovery after reload, Stop during model
generation with a terminal `cancelled` state, a successful follow-up request,
and a dated news answer supported by fetched sources.
