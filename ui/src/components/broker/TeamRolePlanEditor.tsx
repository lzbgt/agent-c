import React from "react";
import FieldLabel from "../FieldLabel";
import RoleGraphPreview, { type RoleGraphEdge } from "./RoleGraphPreview";

type TeamRolePlanEditorProps = {
  disabled?: boolean;
  roleInstructions: Record<string, string>;
  onRoleInstructionsChange: (next: Record<string, string>) => void;
  rolePromptMode: string;
  onRolePromptModeChange: (mode: string) => void;
  edges?: RoleGraphEdge[];
  onEdgesChange?: (next: RoleGraphEdge[]) => void;
  showEdges?: boolean;
  roleOptions?: string[];
};

const normalizeRole = (raw: string) => String(raw || "").trim().toLowerCase();

export default function TeamRolePlanEditor(props: TeamRolePlanEditorProps) {
  const disabled = !!props.disabled;
  const roleInstructions = props.roleInstructions || {};
  const roleEntries = React.useMemo(() => {
    return Object.entries(roleInstructions)
      .map(([role, instruction]) => ({ role: String(role || "").trim(), instruction: String(instruction || "") }))
      .filter((row) => row.role)
      .sort((a, b) => a.role.localeCompare(b.role));
  }, [roleInstructions]);

  const edges = Array.isArray(props.edges) ? props.edges : [];
  const showEdges = props.showEdges !== false;

  const [newRole, setNewRole] = React.useState<string>("");
  const [newInstruction, setNewInstruction] = React.useState<string>("");
  const [roleError, setRoleError] = React.useState<string>("");

  const [edgeFrom, setEdgeFrom] = React.useState<string>("");
  const [edgeTo, setEdgeTo] = React.useState<string>("");
  const [edgeReason, setEdgeReason] = React.useState<string>("");
  const [edgeError, setEdgeError] = React.useState<string>("");
  const listId = React.useId();

  const roleOptions = React.useMemo(() => {
    const set = new Set<string>();
    for (const entry of roleEntries) set.add(normalizeRole(entry.role));
    for (const edge of edges) {
      if (edge?.from_role) set.add(normalizeRole(edge.from_role));
      if (edge?.to_role) set.add(normalizeRole(edge.to_role));
    }
    if (Array.isArray(props.roleOptions)) {
      for (const role of props.roleOptions) set.add(normalizeRole(role));
    }
    return Array.from(set).filter(Boolean).sort();
  }, [roleEntries, edges, props.roleOptions]);

  const updateInstruction = React.useCallback(
    (role: string, instruction: string) => {
      const key = normalizeRole(role);
      if (!key) return;
      const next = { ...roleInstructions };
      const trimmed = String(instruction || "").trim();
      if (trimmed) next[key] = trimmed;
      else delete next[key];
      props.onRoleInstructionsChange(next);
    },
    [props, roleInstructions],
  );

  const handleAddRole = () => {
    const role = normalizeRole(newRole);
    const instruction = String(newInstruction || "").trim();
    if (!role || !instruction) {
      setRoleError("role + instruction required");
      return;
    }
    setRoleError("");
    updateInstruction(role, instruction);
    setNewRole("");
    setNewInstruction("");
  };

  const handleRemoveRole = (role: string) => {
    const key = normalizeRole(role);
    if (!key) return;
    const next = { ...roleInstructions };
    delete next[key];
    props.onRoleInstructionsChange(next);
  };

  const handleAddEdge = () => {
    if (!showEdges || !props.onEdgesChange) return;
    const fromRole = normalizeRole(edgeFrom);
    const toRole = normalizeRole(edgeTo);
    if (!fromRole || !toRole) {
      setEdgeError("from_role and to_role required");
      return;
    }
    setEdgeError("");
    const next: RoleGraphEdge[] = [...edges, { from_role: fromRole, to_role: toRole, reason: edgeReason.trim() || undefined }];
    props.onEdgesChange(next);
    setEdgeFrom("");
    setEdgeTo("");
    setEdgeReason("");
  };

  const handleRemoveEdge = (idx: number) => {
    if (!showEdges || !props.onEdgesChange) return;
    const next = edges.filter((_, i) => i !== idx);
    props.onEdgesChange(next);
  };

  return (
    <div className="grid gap-2">
      <div className="flex flex-wrap items-center gap-2">
        <FieldLabel>Role prompt mode</FieldLabel>
        <select
          className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
          value={props.rolePromptMode}
          onChange={(e) => props.onRolePromptModeChange(e.target.value)}
          disabled={disabled}
        >
          <option value="prepend">prepend</option>
          <option value="append">append</option>
          <option value="replace">replace</option>
        </select>
        <span className="text-[11px] text-white/50">Use {"{{goal}}"} to inject the base prompt.</span>
      </div>

      <div className="grid gap-1">
        <FieldLabel>Role instructions</FieldLabel>
        <div className="flex flex-wrap items-center gap-2">
          <input
            className="min-w-[140px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={newRole}
            onChange={(e) => setNewRole(e.target.value)}
            placeholder="planner"
            list={listId}
            disabled={disabled}
          />
          <input
            className="min-w-[220px] flex-[2] rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={newInstruction}
            onChange={(e) => setNewInstruction(e.target.value)}
            placeholder="You are the planner. Goal: {{goal}}"
            disabled={disabled}
          />
          <button
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            onClick={handleAddRole}
            disabled={disabled}
          >
            Add role
          </button>
        </div>
        {roleError ? <div className="text-[11px] text-rose-200">{roleError}</div> : null}
        <datalist id={listId}>
          {roleOptions.map((role) => (
            <option key={`role-plan-opt-${role}`} value={role} />
          ))}
        </datalist>
        {roleEntries.length > 0 ? (
          <div className="grid gap-2">
            {roleEntries.map((row) => (
              <div
                key={`role-instruction-${row.role}`}
                className="grid gap-1 rounded-md border border-white/10 bg-black/30 p-2"
              >
                <div className="flex items-center justify-between gap-2 text-[11px] text-white/70">
                  <span className="text-white/90">{row.role}</span>
                  <button
                    className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[10px] text-white/70 hover:bg-black/40"
                    type="button"
                    onClick={() => handleRemoveRole(row.role)}
                    disabled={disabled}
                  >
                    Remove
                  </button>
                </div>
                <textarea
                  className="min-h-[64px] w-full rounded-md border border-white/10 bg-black/20 px-2 py-1 text-[11px] text-white/90"
                  value={row.instruction}
                  onChange={(e) => updateInstruction(row.role, e.target.value)}
                  disabled={disabled}
                />
              </div>
            ))}
          </div>
        ) : (
          <div className="text-[11px] text-white/40">No role instructions defined.</div>
        )}
      </div>

      {showEdges && props.onEdgesChange ? (
        <div className="grid gap-1">
          <FieldLabel>Role graph edges</FieldLabel>
          <div className="flex flex-wrap items-center gap-2">
            <input
              className="min-w-[120px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={edgeFrom}
              onChange={(e) => setEdgeFrom(e.target.value)}
              placeholder="planner"
              list={listId}
              disabled={disabled}
            />
            <span className="text-[11px] text-white/60">→</span>
            <input
              className="min-w-[120px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={edgeTo}
              onChange={(e) => setEdgeTo(e.target.value)}
              placeholder="executor"
              list={listId}
              disabled={disabled}
            />
            <input
              className="min-w-[200px] flex-[2] rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={edgeReason}
              onChange={(e) => setEdgeReason(e.target.value)}
              placeholder="handoff reason (optional)"
              disabled={disabled}
            />
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
              type="button"
              onClick={handleAddEdge}
              disabled={disabled}
            >
              Add edge
            </button>
          </div>
          {edgeError ? <div className="text-[11px] text-rose-200">{edgeError}</div> : null}
          {edges.length > 0 ? (
            <div className="grid gap-2">
              {edges.map((edge, idx) => (
                <div
                  key={`role-edge-${edge.from_role}-${edge.to_role}-${idx}`}
                  className="flex flex-wrap items-center justify-between gap-2 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70"
                >
                  <div>
                    <span className="text-white/90">{edge.from_role}</span> →
                    <span className="text-white/90"> {edge.to_role}</span>
                    {edge.reason ? ` · ${edge.reason}` : ""}
                  </div>
                  <button
                    className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[10px] text-white/70 hover:bg-black/40"
                    type="button"
                    onClick={() => handleRemoveEdge(idx)}
                    disabled={disabled}
                  >
                    Remove
                  </button>
                </div>
              ))}
            </div>
          ) : (
            <div className="text-[11px] text-white/40">No role edges defined.</div>
          )}
          <RoleGraphPreview roles={roleOptions} edges={edges} />
        </div>
      ) : null}
    </div>
  );
}
