# Sequential delegated transcript continuation

Implemented source slice, schema v41. No deployment is implied.

Sequential kind:delegate host-model children now use the bounded provider/tool transcript guard. Each ordered child has an internally generated delegate.v1:<index> slot under its real parent workflow/task. Labels do not determine identity. The persisted parent definition binds each child; current policy and session fences remain authoritative.

Completed children replay saved receipts with zero fresh usage and no provider/tool calls. Verified incomplete transcripts replay exact saved decisions and tool receipts. Any child requiring continuation, reporting uncertainty, or observing cancellation stops the chain before later attempts, even with stop_on_ok:false or an earlier successful child. Uncertain effects never authorize a fallback model.

## Limits

Fresh grants remain parent-attempt reservations. Each child receives only the remaining grant; returned fresh usage reduces subsequent availability. Lost reservation charges are not refunded. This slice does NOT bypass exhausted parent admission for replay-only delegates. Existing token, elapsed, step and tool limits remain conservative.

Only the existing host-model guard contracts are inherited. Direct chat, capsules, provider-native tools, unsupported preparation and parallel delegate macro semantics are unchanged. No exactly-once physical-effect guarantee or trusted host receipt resolver is added.

## Persistence

The transactional v40→v41 migration preserves every old effect ID, receipt and revision with an empty legacy slot. Unique ownership becomes (workflow_id,task_id,effect_slot); the foreign key still references the real parent task. Schema version publication is in the same migration transaction. Empty-slot identity hashing is unchanged.

Workflow effect metadata exposes child effect_slot. Projection is complete up to 4096 rows; larger projections fail explicitly instead of silently truncating.

## Verification

workflow_delegate_continuation_smoke.py uses a temporary authenticated one-worker daemon, loopback fake provider and synthetic subprocess writes. It checks three-child crash/restart, no repeated completed-child provider calls, receipted tool reuse, transient retry, unreceipted-operation stop before fallback, finite fresh usage and v40 legacy receipt migration. Existing model-effect/transcript and ordinary recovery regressions remain applicable.

Further work: replay-only parent admission with exhausted fresh grants, comprehensive multi-child persistent-session edit qualification, additional preparation adapters and trusted host receipt reconciliation. These are not claimed by this slice.
