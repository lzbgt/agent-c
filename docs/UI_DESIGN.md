# User attention is the UI's default budget

Agent-C opens as a conversation. The primary content is the user's message and
the agent's response, in chronological order. Real files, media and interactive
results stay available alongside that conversation. A greeting should look like
a greeting, with one user message and one reply.

## Default hierarchy

1. **Messages and results:** readable text, a quiet distinction between You and
   Agent, and a compact composer with Attach and Send. Empty attachment counts,
   duplicate latest-outcome cards, system prompts and raw tool output do not
   occupy the conversation. Existing debug preferences cannot expand its
   metadata automatically.
2. **Subject options:** Settings leads with model, agent capabilities and the
   current conversation. Environments appear there when there is a choice.
   Conversation management is a closed section. Transport, leases, provider
   parameters, memory tuning, limits, team administration and diagnostics live
   under closed Advanced settings sections. Diagnostics are not queried merely
   by opening basic settings.
3. **Execution evidence:** Activity & details groups stored context, tool
   records, timestamps, run settings and technical history. It is closed on
   entry. Message details is an optional composer utility, also closed by
   default. Details remain inspectable without competing with the answer.

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
append-only chat archive. Live events provide streamed assistant text. A known
settled trace replaces its live view, while repeated identical prompts remain
distinct turns. Tool records, system messages and debug events belong to the
detail view.

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
