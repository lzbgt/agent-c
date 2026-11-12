import React from "react";
import { recordWorkflowDecision, type DecisionRequest } from "../../api/workflowDecisions";
import type { WorkflowDecision } from "../../api/schemas/workflow";
import type { ApiAuth } from "../../api/auth";

type Controls = { baseUrl: string; auth?: ApiAuth };
type Props = {
  workflowId: string;
  decisions: WorkflowDecision[];
  active: boolean;
  controls?: Controls;
  onReload: (id: string) => void;
};

function DecisionCard(props: Props & { decision: WorkflowDecision }) {
  const row = props.decision;
  const [note, setNote] = React.useState("");
  const [pending, setPending] = React.useState(false);
  const [error, setError] = React.useState<string | null>(null);
  const [attempt, setAttempt] = React.useState<DecisionRequest | null>(null);
  const [receipt, setReceipt] = React.useState<WorkflowDecision | null>(null);
  const busy = React.useRef(false);
  const mounted = React.useRef(true);
  const abort = React.useRef<AbortController | null>(null);
  React.useEffect(() => {
    mounted.current = true;
    return () => { mounted.current = false; abort.current?.abort(); };
  }, []);
  const current = receipt ?? row;
  const available = props.active && current.state === "waiting" && current.available && Boolean(props.controls);
  const selected = current.choices.find((choice) => choice.id === current.choice_id);
  const attemptedChoice = current.choices.find((choice) => choice.id === attempt?.choice_id);
  const noteTooLong = new TextEncoder().encode(note).length > 4096;

  async function choose(choiceId: string) {
    if (busy.current || !available || !props.controls || noteTooLong) return;
    // After an uncertain response, preserve the exact delivery and payload.
    if (attempt && attempt.choice_id !== choiceId) return;
    const request: DecisionRequest = attempt ?? {
      workflow_id: props.workflowId, task_id: row.task_id, decision_id: row.decision_id,
      choice_id: choiceId, idempotency_key: "ui-" + crypto.randomUUID(), note,
    };
    busy.current = true; setPending(true); setError(null); setAttempt(request);
    const controller = new AbortController(); abort.current = controller;
    try {
      const result = await recordWorkflowDecision(props.controls.baseUrl, request, props.controls.auth, controller.signal);
      if (!mounted.current) return;
      setReceipt(result.decision);
      props.onReload(props.workflowId);
    } catch (cause) {
      if (mounted.current) setError(cause instanceof Error ? cause.message : String(cause));
    } finally {
      busy.current = false;
      if (mounted.current) setPending(false);
    }
  }

  return <article className="min-w-0 rounded-lg border border-white/10 bg-black/20 p-3" data-testid="workflow-decision-card">
    <div className="flex flex-wrap items-center justify-between gap-2 text-[11px] text-white/55">
      <span className="break-all">{row.task_id}</span>
      <span>{current.state === "resolved" ? "Choice recorded" : current.state === "expired" ? "Decision expired" :
        current.state === "stopped" || !props.active ? "Workflow stopped" : "Waiting for a choice"}</span>
    </div>
    <h3 className="mt-2 break-words text-sm font-semibold text-white/90">{current.prompt}</h3>
    {current.state === "resolved" ? <div className="mt-2 break-words text-xs text-emerald-100" role="status">
      {selected?.label ?? current.choice_id} · {current.outcome === "continue" ? "Continue outcome recorded" : "Fail outcome recorded"}
      {current.note ? <p className="mt-1 whitespace-pre-wrap text-white/60">{current.note}</p> : null}
    </div> : null}
    {available ? <>
      <label className="mt-3 block text-xs text-white/60">
        Note for {row.task_id} (optional)
        <textarea aria-label={`Decision note for ${row.task_id}`} value={note} disabled={pending || attempt !== null}
          onChange={(event) => setNote(event.target.value)} rows={2} maxLength={4096}
          className="mt-1 block w-full min-w-0 rounded border border-white/10 bg-black/20 p-2 text-xs disabled:opacity-60" />
      </label>
      {noteTooLong ? <p role="alert" className="mt-1 text-xs text-amber-100">Note must fit within 4096 UTF-8 bytes.</p> : null}
      {error ? <div role="alert" className="mt-2 break-words text-xs text-amber-100">
        <p>{error}</p>
        <p className="mt-1">Could not confirm the receipt. Refresh it or retry the same choice safely. No different choice will be sent.</p>
      </div> : null}
      <div className="mt-3 flex flex-wrap gap-2">
        {attempt && error ? <button type="button" className="workspace-button disabled:opacity-50" disabled={pending}
          onClick={() => void choose(attempt.choice_id)}>Retry same choice: {attemptedChoice?.label ?? attempt.choice_id}</button> :
          current.choices.map((choice) => <button key={choice.id} type="button"
            className={`workspace-button max-w-full whitespace-normal text-left disabled:opacity-50 ${choice.outcome === "fail" ? "text-rose-100" : "text-emerald-100"}`}
            disabled={pending || attempt !== null || noteTooLong} onClick={() => void choose(choice.id)}>
            <span className="block break-words">{choice.label}</span>
            <span className="block text-[10px] opacity-65">{choice.outcome === "continue" ? "Continue this step" : "Fail this step; block dependent steps"}</span>
          </button>)}
        {error ? <button type="button" className="workspace-button" disabled={pending} onClick={() => props.onReload(props.workflowId)}>Refresh receipt</button> : null}
      </div>
      {pending ? <p role="status" className="mt-2 text-xs text-white/60">Recording choice…</p> : null}
    </> : current.state === "waiting" && props.active ? <p className="mt-2 text-xs text-white/50">Refresh to check whether this decision is accepting input.</p> : null}
    {current.expires_unix_ms ? <p className="mt-2 text-[11px] text-white/45">Deadline: {new Date(current.expires_unix_ms).toLocaleString()}</p> : null}
  </article>;
}

export default function WorkflowDecisionsSection(props: Props) {
  if (!props.decisions.length) return null;
  // A connection or credential change unmounts old requests. React keys are not rendered or persisted.
  const scope = JSON.stringify([props.workflowId, props.controls?.baseUrl, props.controls?.auth]);
  return <section aria-label="Process decisions" className="grid min-w-0 gap-2" data-testid="workflow-decisions">
    <h2 className="text-sm font-semibold text-white/80">Process decisions</h2>
    <p className="text-[11px] leading-relaxed text-white/50">Choose only an outcome defined by this process. Waiting does not occupy a worker. A choice does not grant extra tool permissions.</p>
    {props.decisions.map((decision) => <DecisionCard key={scope + decision.decision_id} {...props} decision={decision} />)}
  </section>;
}
