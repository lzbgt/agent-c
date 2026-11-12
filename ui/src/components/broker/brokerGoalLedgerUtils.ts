export type GoalLedgerBinding = { agent_id: string; deployment_id: string; session_id: string };
export function record(value: unknown): Record<string, unknown> | null {
  return value !== null && typeof value === "object" && !Array.isArray(value) ? value as Record<string, unknown> : null;
}
export function goalLedgerBindingError(value: unknown): string | null {
  const binding = record(value);
  if (!binding || Object.keys(binding).length !== 3) return "Requirements need explicit agent, deployment and session IDs.";
  for (const key of ["agent_id", "deployment_id", "session_id"]) {
    const id = binding[key];
    if (typeof id !== "string" || !/^[A-Za-z0-9_.-]{1,200}$/.test(id) || id.includes("..") || id === ".") return "Requirements need valid agent, deployment and session IDs.";
  }
  return null;
}
export const goalLedgerReason: Record<string, string> = {
  owner_waiting: "Waiting by owner request",
  working: "Working on unfinished requirements",
  dispatching: "Submitting one requirement turn",
  requirements_ready: "Unfinished requirements ready to continue",
  requirements_blocked: "Waiting on affected requirements",
  no_requirement_progress: "Waiting for verified requirement progress",
  requirements_verified: "All requirements verified",
  authority_unavailable: "Waiting for the requirement ledger",
  authority_changed: "Authority changed; reconciliation required",
  authority_history_changed: "Ledger identity or history changed; reconciliation required",
  invalid_binding: "Invalid requirement authority",
  dispatch_outcome_unknown: "Dispatch outcome unknown; do not retry",
  unbound_active_run: "Active run is not bound to this ledger",
};
