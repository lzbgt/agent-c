import React from "react";
import { apiReconcileWorkflowEffect } from "../../api/workflow";
import type { ApiAuth } from "../../api/auth";

export default function DeviceReceiptButton(props: {
  workflowId: string;
  taskId: string;
  controls: { baseUrl: string; auth?: ApiAuth };
  onReload: (id: string) => void;
}) {
  const [pending, setPending] = React.useState(false);
  const [error, setError] = React.useState<string | null>(null);
  const busy = React.useRef(false);
  const mounted = React.useRef(true);
  React.useEffect(() => {
    mounted.current = true;
    return () => { mounted.current = false; };
  }, []);

  async function checkReceipt() {
    if (busy.current) return;
    busy.current = true;
    setPending(true);
    setError(null);
    try {
      const result = await apiReconcileWorkflowEffect(props.controls.baseUrl, {
        workflow_id: props.workflowId, task_id: props.taskId,
      }, props.controls.auth);
      if (result.ok === false) throw new Error(result.error ?? "Device receipt could not be checked");
      if (mounted.current) props.onReload(props.workflowId);
    } catch (cause) {
      if (mounted.current) setError(cause instanceof Error ? cause.message : String(cause));
    } finally {
      busy.current = false;
      if (mounted.current) setPending(false);
    }
  }

  return <div className="mt-2">
    <button type="button" className="workspace-button disabled:opacity-50"
      disabled={pending} onClick={() => void checkReceipt()}>
      {pending ? "Checking…" : "Check device receipt"}
    </button>
    {error ? <p className="mt-2 break-words text-xs text-amber-100" role="alert">{error}</p> : null}
  </div>;
}
