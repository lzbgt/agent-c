# CLI event and artifact contract

`agent exec`, `agent run` and `agent resume <session> <prompt>` support `--json` for integration with production pipelines. The CLI uses the existing provider/tool executor, completion guard, policies and budgets. It adds no permission round trip. This contract draws on the machine-output and final-message surfaces in [Codex non-interactive mode](https://learn.chatgpt.com/docs/non-interactive-mode); the Agent-C event format has its own version and semantics.

```sh
./build/agent exec "inspect, implement and verify the change" \
  -C /path/to/project --json -o ./verified-result.txt > ./run-events.jsonl
```

Options follow the command. `--output-last-message <path>` and `-o <path>` are equivalent. These options require a non-interactive prompt; `chat` and promptless `resume` reject them. `--help` retains normal help output. `--json` requires jsoncpp. `--quiet` suppresses the existing diagnostic transcript while machine events continue.

## Envelope and lifecycle

Each stdout line is one complete JSON object described by [cli_event_v1.schema.json](cli_event_v1.schema.json):

```json
{"schema":"agentd.cli.event.v1","sequence":1,"type":"run.started","data":{"command":"run","model":"stub","tools":"host","session_id":"default","cwd":"/project"}}
```

`sequence` starts at one and increases for this CLI process. Consumers supply their own invocation identity when combining streams from multiple processes; the sequence is not a persistent run/session ID. Stderr remains a diagnostic channel. No plain assistant text, progress formatting or ANSI escapes are mixed into JSON stdout.

| Event | Meaning |
| --- | --- |
| `run.started` | Configuration/session setup and output-file preflight succeeded. Includes normalized command, selected model/tools, actual cwd and session ID (`null` for ephemeral runs). |
| `assistant.delta` | Optional provisional text when `--stream-assistant` is used. Common type for tools and no-tools execution; source step/epoch may be present. |
| `loop.*` | Existing best-effort loop progress, such as tool calls/results, retries, compaction, proposed messages, limits and completion continuation. Nested payload fields may evolve. |
| `assistant.completed` | The executor accepted its final assistant text after any completion guard. Includes `data.text`. Session/artifact publication can still fail afterward. |
| `run.completed` | Terminal success, after session persistence and requested last-message publication. Includes `exit_code: 0`. |
| `run.failed` | Terminal failure with nonzero `exit_code` and `error.code`/`error.message`. Setup/argument failure can emit this without `run.started`. |

Use the terminal `run.completed` **and** successful process exit as the pipeline success condition. `loop.done`, a proposed assistant message and a streamed delta cannot establish success. A normally returning invocation emits exactly one terminal event. A process kill, broken pipe or machine failure can leave no terminal event and must be treated as interrupted. This stream is live delivery; it is not a durable checkpoint or an automatic resumption journal.

`run.completed.data.usage`, when available for tool execution, contains measured `steps_executed` and `tool_calls_executed`; it does not invent token usage for unsupported provider paths. `capture` describes the bounded in-memory event log: `truncated`, `events` and serialized `bytes`. The captured log is capped at 4,000 events and 2 MiB plus a small terminal summary. Live events continue after either capture limit. File-backed audit persistence retains its separate record-size/retention limits and remains best-effort.

Raw model request/response events are excluded from JSON stdout. Tool arguments/results and assistant text can still contain application data. `--transcript-jsonl` retains the older explicit raw event export, independently of the versioned stdout envelope.

## Final-message file

The file contains the exact accepted final text, without an added newline. Relative paths resolve under `-C`/`--cd`. The parent directory must exist. The CLI creates an exclusive temporary file beside the destination before making a provider request. It writes and closes that file, then renames it into place only after execution and session saving succeed. It never deletes an existing destination to work around a failed replacement.

Failed execution, false completion and failed publication leave the existing destination untouched by this output writer. Concurrent successful publishers use independent temporary files; the last successful rename wins. A process crash can leave a temporary file for cleanup. Rename atomicity/durability depends on the filesystem/platform; no disk power-loss certification is claimed. On platforms where rename cannot replace an existing file, publication fails rather than deleting the previous result.

`run.completed.data.last_message_file` reports the absolute published path. Publication failure produces exit 1 and `error.code: output_write_failed`, even if an `assistant.completed` event was already emitted. Normal text mode still prints the assistant response while optionally publishing the file.

## Verification

`cli_machine_output_smoke` runs the real CLI against a local HTTP/SSE service. It checks sequence/lifecycle, Unicode/newline escaping, resume metadata, tool output, streamed/no-tool/multimodal paths, argument and provider failures, last-message preflight/replacement/cleanup, a 608,000-character coalesced stream beyond the audit event cap and large messages that exhaust the separate byte cap. `cli_harness_smoke` also verifies that premature completion cannot publish a successful event or replace a verified artifact.

The stream test found and covers two pre-existing truncation paths: the HTTP SSE client retained excess events without draining after a full parser batch, and host audit capture silenced live callbacks after 4,000 events. The parser now supports empty-feed draining, its C decoder drains bounded batches, and HTTP/voice consumers drain their parser batches. Portable C tests cover split input and coalesced input with small batch sizes. This establishes packet-boundary independence for the checked fixtures; it does not establish live-provider compatibility or embedded hardware timing.
