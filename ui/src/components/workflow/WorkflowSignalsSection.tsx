import React from "react";
import type { WorkflowSignal } from "../../api/schemas/workflow";
import { formatUnixMs } from "./workflowPanelUtils";

export default function WorkflowSignalsSection({ signals }: { signals: WorkflowSignal[] }) {
  if (!signals.length) return null;
  return <section className="min-w-0 rounded-md border border-sky-300/30 bg-sky-300/5 p-3" aria-label="External signals">
    <h2 className="text-sm font-semibold text-sky-100">External signals</h2>
    <p className="mt-1 text-xs text-white/60">A connected system supplies one result for each activated wait. Waiting releases workers; delivery does not grant tool permissions.</p>
    {signals.map(signal => <div key={signal.signal_id} className="mt-3 min-w-0 text-sm">
      <p className="break-words font-medium">{signal.title || signal.name}</p>
      <p className="break-words">{signal.state === "waiting" ? (signal.available ? "Waiting for input" : "Input not currently available") :
        signal.state === "resolved" ? "Input received" : signal.state === "expired" ? "Expired without input" : "Stopped"}</p>
      {signal.expires_unix_ms && signal.state === "waiting" ? <p className="text-xs text-white/60">Expires {formatUnixMs(signal.expires_unix_ms)}</p> : null}
      <details className="mt-1 text-xs text-white/60">
        <summary className="cursor-pointer">Delivery details</summary>
        <dl className="mt-2 space-y-1 break-all">
          <div><dt>Name</dt><dd>{signal.name}</dd></div>
          <div><dt>Workflow</dt><dd>{signal.workflow_id}</dd></div>
          <div><dt>Task</dt><dd>{signal.task_id}</dd></div>
          <div><dt>Signal identity</dt><dd>{signal.signal_id}</dd></div>
          {signal.payload_hash ? <div><dt>Recorded payload hash</dt><dd>{signal.payload_hash}</dd></div> : null}
        </dl>
        <p className="mt-2">Use the authenticated workflow signal API with this exact identity, a stable delivery key and a JSON object up to 16 KiB. After a lost response, read the original receipt or retry only the identical delivery. This panel never submits input.</p>
      </details>
    </div>)}
  </section>;
}
