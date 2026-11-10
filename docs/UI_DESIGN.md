# User attention is the UI's default budget

Agent-C opens as a conversation. The primary content is the user's message and
the agent's response, in chronological order. Real files, media and interactive
results stay available alongside that conversation. A greeting should look like
a greeting, with one user message and one reply.

## Default hierarchy

1. **Messages and results:** readable text, a quiet distinction between You and
   Agent, and a compact composer with Attach and Send. Empty attachment counts,
   duplicate latest-outcome cards, system prompts and raw tool output do not
   occupy the conversation. The details view never renders user/assistant rows,
   copied per-run answers, prompt previews, a second team transcript or raw
   provider payloads containing messages. Existing debug preferences cannot
   restore those duplicate renderers or expand metadata automatically.
2. **Subject options:** Settings leads with model, agent capabilities and the
   current conversation. Environments appear there when there is a choice.
   Conversation management is a closed section. Transport, leases, provider
   parameters, memory tuning, limits, team administration and diagnostics live
   under closed Advanced settings sections. Diagnostics are not queried merely
   by opening basic settings.
3. **Execution evidence:** Activity & details groups system context, tool
   records, timestamps, run settings and technical history. It is closed on
   entry. The composer has no duplicate attachment/metadata drawer or page-wide
   overlay; attachments appear once as removable chips. Details remain inspectable without competing with the answer.

Errors remain visible in plain language. Execution and authorized browser actions
continue independently of whether their detail sections are open. Collapsing
information is presentation, not a permission boundary or an execution pause.
The composer clears an accepted draft only if the user has not started another
draft. IME composition does not submit a message.

## Transcript sources

Run audit entries provide user turns and assistant outputs without exposing
model context. The newer native DB snapshot is used while the audit query catches
up, and is also the fallback for connectors without audit entries. Native model
context rows share their producing run's timestamp; they are not a permanent
append-only chat archive. Live events provide streamed assistant text through the same primary renderer. A known
settled trace replaces its live view, while repeated identical prompts remain
distinct turns. Tool records, system messages and debug events belong to the
detail view. Browser action handling receives the full event stream independently
of the activity renderer; removing duplicate messages does not filter execution
inputs.

The UI displays the bounded history provided by the current APIs. A dedicated
append-only conversation protocol with stable message IDs, pagination and
submission reconciliation remains work; do not infer those guarantees from this
presentation change.

## Responsive acceptance

Desktop conversation text and its composer share a readable column. Phones use
the viewport width with a compact header and touch navigation. Settings stays a
modal with focus containment, Escape dismissal and focus restoration.

`ui/e2e/chat_focus.spec.ts` checks a greeting at desktop/390/320px, duplicate-free
rendering, repeated identical turns, streaming handoff, defaults for settings,
accepted sends, IME behavior and automatic browser effects while metadata is
closed. Existing responsive tests cover tablet layout, workflow tools and a
shortened viewport. Live browser probes qualify the deployed build separately;
these are browser checks, not physical phone keyboard certification.

## Tool pages

Memory separates saved knowledge from summaries. Values and summary text are
readable content; provenance, versions, scoring, paths and response JSON belong
to details. Key prefix is a prefix filter, not a full-text search. Query limits,
indexing, trace correlation, scheduling and cleanup stay closed. Merely opening
Memory makes no memory maintenance requests. Preview selects summary inputs
without a model call or write; generating and saving are distinct explicit actions.

Voice leads with a clear call state, microphone action and compact speaker,
camera, image and end controls. Shared images have a visible remove action.
Captions use the same primary chat renderer once per utterance/reply; text and
audio-transcript events cannot create duplicate messages. There is no diagnostic
polling or transport JSON on the default page. Ending/leaving a call releases its
devices, audio processors, WebSocket and timers. See [VOICE.md](VOICE.md).

Run comparison leads with the loaded runs' messages and outputs. Evidence,
baselines, hashes, usage and tool-record differences stay behind closed controls.
Trace lookup shows messages and failures while grouping transport evidence.
Policy decisions keep status, action identity and decision controls visible;
additional filters and request metadata stay closed. Workflow rows keep status
and their action primary; lookup knobs, timeline and copies of transport IDs stay
in details.

Labels and layout explain the default pages. Connection errors offer a next
action without backend flags, protocol setup instructions, developer tokens or
implementation roadmaps. Architecture guidance belongs in repository docs.

`memory_panel.spec.ts`, `tool_focus.spec.ts`, `voice_panel.spec.ts`,
`run_diff_panel.spec.ts`, `broker_trace_lookup.spec.ts` and
`approval_queue_panel.spec.ts` exercise default visibility, real content,
requested advanced actions and mobile overflow. The chat checks also expand
activity and team setup to verify that messages still occur once.

## Presentation ownership

Scripted canvas and DOM results share a per-entity browsing context. Their
`window`, `document`, keyboard listeners, CSS, animation frames and timers belong
to that presentation. Canvas focus is visible and follows pointer/keyboard
navigation. Composer keyboard events remain within the composer. Unrelated
React updates and snapshots with equal execution inputs preserve the document;
changed execution inputs create a new document, and unmounting disposes it.

The shared runtime retains the existing trusted script and authenticated
artifact API. Its same-origin frame scopes events and lifecycle; it is not a
security boundary against hostile code. DOM query/evaluation for presentation
content must target the frame's document rather than the workspace document.

`scene_scope.spec.ts` checks continuous animation during typing, sibling canvas
focus, composer submission, equal-value polling, explicit source updates, draft
restoration, detached animation/timer cleanup and DOM style/input separation.
