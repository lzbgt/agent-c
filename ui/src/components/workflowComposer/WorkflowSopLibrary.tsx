import React from "react";
import type { ApiAuth } from "../../api/auth";
import { listWorkflowSops, saveWorkflowSop, type WorkflowSopDefinition, type WorkflowSopReference } from "../../api/workflowSops";

type Props = {
  baseUrl: string;
  auth?: ApiAuth;
  composerJson: string;
  disabled: boolean;
  onUse: (ref: WorkflowSopReference) => void;
};

// The parent keys this component by connection/auth identity. Requests are aborted
// on unmount and guarded against stale completions, including uncancellable mocks.
export default function WorkflowSopLibrary({ baseUrl, auth, composerJson, disabled, onUse }: Props) {
  const [rows, setRows] = React.useState<WorkflowSopDefinition[]>([]);
  const [next, setNext] = React.useState<number>();
  const [loading, setLoading] = React.useState(false);
  const [saving, setSaving] = React.useState(false);
  const [error, setError] = React.useState("");
  const [message, setMessage] = React.useState("");
  const active = React.useRef(true);
  const busy = React.useRef(false);
  const generation = React.useRef(0);
  const requests = React.useRef(new Set<AbortController>());
  let sop: unknown;
  try {
    const parsed = JSON.parse(composerJson);
    if (parsed && typeof parsed === "object" && !Array.isArray(parsed)) sop = parsed.sop;
  } catch { /* The editor owns parse diagnostics. */ }

  async function load(before?: number) {
    if (busy.current) return;
    busy.current = true;
    const gen = ++generation.current;
    const controller = new AbortController();
    requests.current.add(controller);
    setLoading(true);
    setError("");
    try {
      const page = await listWorkflowSops(baseUrl, auth, before, controller.signal);
      if (!active.current || gen !== generation.current) return;
      setRows((old) => before ? [...old, ...page.definitions.filter((r) => !old.some((x) => x.registry_id === r.registry_id))] : page.definitions);
      setNext(page.next_before_id);
    } catch (e) {
      if (active.current && gen === generation.current) setError(e instanceof Error ? e.message : String(e));
    } finally {
      requests.current.delete(controller);
      if (active.current && gen === generation.current) { busy.current = false; setLoading(false); }
    }
  }

  React.useEffect(() => {
    active.current = true;
    busy.current = false;
    void load();
    return () => {
      active.current = false;
      generation.current++;
      for (const request of requests.current) request.abort();
      requests.current.clear();
    };
    // Connection changes remount this component; changing editor text must not reload.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  async function save() {
    if (busy.current || !sop || disabled) return;
    busy.current = true;
    const gen = ++generation.current;
    const controller = new AbortController();
    requests.current.add(controller);
    setSaving(true);
    setError("");
    setMessage("");
    let saved = false;
    try {
      const result = await saveWorkflowSop(baseUrl, sop, auth, controller.signal);
      if (!active.current || gen !== generation.current) return;
      setMessage(`${result.created ? "Saved" : "Already saved"} ${result.definition.id} · ${result.definition.version}. Nothing has run.`);
      saved = true;
    } catch (e) {
      if (active.current && gen === generation.current) {
        setError(`${e instanceof Error ? e.message : String(e)} If the save outcome is uncertain, refresh before retrying. An identical version can be saved safely again.`);
      }
    } finally {
      requests.current.delete(controller);
      if (active.current && gen === generation.current) { busy.current = false; setSaving(false); }
    }
    if (saved && active.current && gen === generation.current) void load();
  }

  const blocked = disabled || loading || saving;
  return (
    <details className="mt-4 min-w-0 text-xs text-white/70" data-testid="workflow-sop-library">
      <summary className="cursor-pointer">Saved SOP versions</summary>
      <p className="mt-2">Save the editor's SOP without running it. Versions are immutable; change the version to save edits.</p>
      <div className="mt-2 flex flex-wrap gap-2">
        <button type="button" className="rounded border border-white/20 px-2 py-1 disabled:opacity-40" disabled={blocked || !sop} onClick={() => void save()}>
          {saving ? "Saving SOP…" : "Save SOP version"}
        </button>
        <button type="button" className="rounded border border-white/20 px-2 py-1 disabled:opacity-40" disabled={blocked} onClick={() => void load()}>Refresh SOPs</button>
      </div>
      <p role="status" className="mt-2 break-words">{loading ? "Loading SOPs…" : message}</p>
      {error ? <p role="alert" className="mt-2 break-words text-amber-200">{error}</p> : null}
      {!loading && !error && rows.length === 0 ? <p>No saved SOP versions yet.</p> : null}
      <ul className="mt-2 space-y-2">
        {rows.map((row) => <li key={row.registry_id} className="flex min-w-0 flex-wrap items-center justify-between gap-2 rounded border border-white/10 p-2">
          <div className="min-w-0 flex-1 break-all">
            <div>{row.title || row.id}</div>
            <div className="text-white/50">{row.id} · {row.version}</div>
            <div className="font-mono text-[10px] text-white/40" title={row.definition_hash}>{row.definition_hash.slice(0, 23)}…</div>
          </div>
          <button type="button" className="shrink-0 rounded border border-white/20 px-2 py-1 disabled:opacity-40" disabled={blocked} aria-label={`Use SOP ${row.id} version ${row.version}`}
            onClick={() => {
              onUse({ id: row.id, version: row.version, definition_hash: row.definition_hash });
              setMessage(`Loaded ${row.id} · ${row.version}. Review the reference, then submit to run.`);
            }}>Use version</button>
        </li>)}
      </ul>
      {next ? <button type="button" className="mt-2 rounded border border-white/20 px-2 py-1 disabled:opacity-40" disabled={blocked} onClick={() => void load(next)}>Load more SOPs</button> : null}
      <p className="mt-2 text-white/50">Use version replaces the editor with a pinned reference. Submit separately to run. Saved SOPs are not a secret store; use server-side credential references.</p>
    </details>
  );
}
