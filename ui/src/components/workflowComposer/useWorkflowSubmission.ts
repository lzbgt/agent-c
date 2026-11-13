import React from "react";
import { apiSubmitWorkflow } from "../../api/workflow";
import { daemonFetchInit, type ApiAuth } from "../../api/auth";
import type { WorkflowSubmitRequest } from "../../workflowTypes";
type Receipt = { idempotency_key: string; session_id: string; created_unix_ms: number };
const prefix = "agentui.pendingWorkflowSubmission.";
function read(key: string): Receipt | null {
  const raw = localStorage.getItem(key);
  if (!raw) return null;
  const value = JSON.parse(raw);
  if (!value || typeof value.idempotency_key !== "string" || !value.idempotency_key ||
      typeof value.session_id !== "string" || typeof value.created_unix_ms !== "number") {
    throw new Error("Stored submission receipt is invalid. Preserve it and inspect the original connection before submitting again.");
  }
  return value;
}
function removeExact(key: string, receipt: Receipt) {
  const current = read(key);
  if (current?.idempotency_key === receipt.idempotency_key && current.created_unix_ms === receipt.created_unix_ms) localStorage.removeItem(key);
}
export default function useWorkflowSubmission(base: string, authKey: string | undefined, auth: ApiAuth | undefined,
  onError: (message: string | null) => void) {
  const identity = JSON.stringify([base.trim(), authKey || "", auth?.mode || "direct", auth?.mode === "broker" ? auth.deploymentId || "" : ""]);
  const current = React.useRef(identity);
  current.current = identity;
  const mounted = React.useRef(true);
  const lock = React.useRef(false);
  const [scope, setScope] = React.useState<{ identity: string; key: string } | null>(null);
  const [pending, setPending] = React.useState<Receipt | null>(null);
  const [busy, setBusy] = React.useState(false);
  React.useEffect(() => { mounted.current = true; return () => { mounted.current = false; }; }, []);
  React.useEffect(() => {
    let active = true;
    setScope(null); setPending(null);
    void (async () => {
      try {
        const bytes = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(identity));
        const key = prefix + Array.from(new Uint8Array(bytes)).map(b => b.toString(16).padStart(2, "0")).join("");
        const receipt = read(key);
        if (active) { setScope({ identity, key }); setPending(receipt); }
      } catch {
        if (active) onError("Submission recovery storage is unavailable. No request will be sent until it can be recorded safely.");
      }
    })();
    return () => { active = false; };
  }, [identity, onError]);
  const fresh = () => mounted.current && current.current === identity;
  const ready = scope?.identity === identity;
  const submit = async (payload: WorkflowSubmitRequest) => {
    if (lock.current || !ready || !scope) return null;
    lock.current = true; setBusy(true);
    let receipt: Receipt | null = null;
    let dispatched = false;
    try {
      const existing = read(scope.key);
      if (existing) {
        setPending(existing);
        onError("A previous submission is unresolved. Check its receipt before creating another run.");
        return null;
      }
      const key = typeof payload.idempotency_key === "string" && payload.idempotency_key.trim()
        ? payload.idempotency_key.trim() : crypto.randomUUID();
      receipt = { idempotency_key: key,
        session_id: payload.allow_sessions === true && typeof payload.session_id === "string" ? payload.session_id.trim() : "",
        created_unix_ms: Date.now() };
      // Synchronous persistence precedes network dispatch. Never store the request body or credentials.
      localStorage.setItem(scope.key, JSON.stringify(receipt));
      if (read(scope.key)?.idempotency_key !== key) throw new Error("receipt persistence failed");
      setPending(receipt);
      dispatched = true;
      const result = await apiSubmitWorkflow(base, { ...payload, idempotency_key: key }, auth);
      if (result.ok && result.workflow_id) {
        if (!fresh()) return null;
        removeExact(scope.key, receipt);
        setPending(null);
        return result;
      }
      if (fresh()) onError((result.error || "No acceptance receipt returned.") + " Submission remains unresolved; check its receipt. Nothing will be retried automatically.");
    } catch (error) {
      if (fresh()) onError(dispatched
        ? "Submission outcome is unknown. Check its receipt; do not submit again. " + String(error)
        : "No request sent: submission receipt could not be persisted. " + String(error));
    } finally {
      lock.current = false;
      if (mounted.current) setBusy(false);
    }
    return null;
  };
  const reconcile = async () => {
    if (lock.current || !ready || !scope) return null;
    lock.current = true; setBusy(true);
    try {
      const receipt = read(scope.key);
      if (!receipt) { if (fresh()) setPending(null); return null; }
      const query = new URLSearchParams({ idempotency_key: receipt.idempotency_key, session_id: receipt.session_id, include_tasks: "0" });
      const response = await fetch(base + "/api/v1/workflow?" + query, daemonFetchInit(auth));
      const result = await response.json();
      if (!response.ok || !result.ok || !result.workflow?.workflow_id) {
        if (fresh()) onError(response.status === 404
          ? "No receipt found yet. The original request may still be in flight. No retry was sent."
          : "Cannot verify the original submission on this connection. The receipt is retained; no retry was sent.");
        return null;
      }
      if (result.workflow.idempotency_key !== receipt.idempotency_key ||
          (result.workflow.session_id || "") !== receipt.session_id) throw new Error("receipt identity mismatch");
      if (!fresh()) return null;
      removeExact(scope.key, receipt);
      setPending(null); onError(null);
      return { ok: true, workflow_id: result.workflow.workflow_id as string, deduped: true };
    } catch (error) {
      if (fresh()) onError("Receipt lookup failed; no execution request was sent. " + String(error));
      return null;
    } finally { lock.current = false; if (mounted.current) setBusy(false); }
  };
  const forget = () => {
    if (lock.current || !ready || !scope || !pending) return;
    if (!window.confirm("Forget this local receipt only? The original workflow may still run. This does not cancel it. Submitting again may duplicate its work. Continue only after checking the original run.")) return;
    try { removeExact(scope.key, pending); setPending(null); onError("Receipt forgotten locally. The original workflow was not cancelled."); }
    catch { onError("Cannot remove the local receipt; no request was sent."); }
  };
  return { submit, reconcile, forget, pending: ready ? pending : null, busy, blocked: !ready || !!pending };
}
