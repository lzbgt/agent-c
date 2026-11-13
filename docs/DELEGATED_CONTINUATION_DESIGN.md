# Delegated transcript continuation: implementation contract

Status: design only, 2026-11-13. No delegated continuation is implemented by this document.
Baseline: 723f30e. Source work remains authorized; production/service/publication holds remain unchanged.

## Verified gap

- workflow_engine.cpp delegate branch calls run_request_to_json_internal_cancellable without a WorkflowModelEffect.
- WorkflowModelEffect hashes only workflow_id/task_id into its mfx_ identity.
- Schema version 40 retains workflow_effects UNIQUE(workflow_id, task_id) and a foreign key to the real parent workflow task.
- A different effect hash alone cannot admit another child: prepare_workflow_effect uses INSERT ON CONFLICT DO NOTHING then reads by effect_id, so the second child fails.
- Synthetic child task IDs break parent ownership and cancellation/reconciliation contracts. Do not use them.
- Delegate accounting subtracts returned usage from fresh remaining grants, aggregates attempt output, and continues to later attempts after failure. It currently has no explicit stop-on-unresolved-child boundary.
- Budget minimum admission precedes the delegate loop. Ordinary model checkpoints can lower fresh tool/step minimum to zero; child checkpoints are not currently considered.
- Effect projection lists at most 128 rows and uses task_id when correlating task status. Multiple children require explicit projection semantics, not hidden truncation.

## Coherent implementation slice

1. Migrate v40 to a schema supporting an explicit effect slot under the unchanged real parent task foreign key.
   Preserve existing effect IDs, receipts, revisions and ownership with an empty legacy slot. Unique key becomes workflow_id/task_id/slot.
   Slot is generated internally from a protocol version plus immutable ordered attempt index, not arbitrary user attempt labels. Duplicate labels must not collide.
   No request parameter may supply an effect ID, ownership tuple or slot.

2. Extend the model guard's constructor with an internal optional slot.
   Empty slot retains byte-for-byte historical identity hashing. Nonempty slot participates in identity hashing; the original persisted parent specification and exact child definition bind the request hash.
   Mutable remaining grants must not become the immutable definition. Runtime authority/configuration continues through the existing guard's scope checks.

3. Preflight the ordered children before fresh resource admission.
   Completed child receipts can be reused without a provider/tool call. Checkpoints may progress only within newly admitted grants.
   Fresh call consumption and historical original usage remain separate. Never credit cached usage as fresh spending, or treat lost reservations as refunded.
   Exhausted grants may permit pure receipt replay but never a fresh call. Provider, tool, elapsed and token constraints retain conservative semantics.

4. Execute each eligible child with its own guard and current policy/session fences.
   Parent cancellation and deadlines apply before every child and at each tool/provider boundary.
   Any unresolved effect or integrity/session conflict stops the delegate chain immediately, even when stop_on_ok is false or an earlier child succeeded.
   Do not silently try another model/provider after an uncertain host operation.
   Define persistent-session chaining explicitly: an earlier completed child receipt may be reused without recommitting its conversation; a later child's session fence must still validate.

5. Publish complete child-scoped evidence in API/UI.
   Include stable internal slot/index and display label separately. Preserve parent task ownership.
   Avoid silently omitting children through the current 128-row projection cap.
   Recovery may observe original receipts; it cannot override a stopped parent or synthesize a successful outcome.

## Required verification before claiming support

- v40 migration preserves every legacy effect receipt and foreign-key cascade; unchanged ordinary tasks replay old IDs.
- Distinct child indexes under one parent admit distinct records; duplicate labels do not collide; changed parent/child definitions fail closed.
- Real temporary-daemon crash after child one completed and during child two; restart reuses exact saved decisions/tool receipts without duplicate effects.
- Unreceipted tool interruption prevents all later attempts, including stop_on_ok=false.
- Finite grants, exhausted fresh budget, replay-only finish, conservative lost charges, independent workflow progress with one worker.
- Cancellation/deadline, session edit/delete/recreate and partial conversation commit rejection.
- Accurate parent aggregate result/usage and complete child-scoped API projection after restart.
- Existing ordinary transcript, model session, image prefetch, budget and effect-reconciliation regressions.

This is a substantial migration plus runtime integration, not a safe one-line wrapper. No exactly-once physical-effect guarantee is proposed.
