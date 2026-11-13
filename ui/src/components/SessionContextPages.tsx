import React from "react";
import { apiGetSessionPage, SessionPageError, type SessionPage } from "../api/sessionPages";
import type { ApiAuth } from "../api";

export default function SessionContextPages(props: { base: string; sessionId: string; auth: ApiAuth }) {
  // Remount on connection/session changes in the caller. Never combine snapshots.
  const [opened, setOpened] = React.useState(false);
  const [page, setPage] = React.useState<SessionPage | null>(null);
  const [busy, setBusy] = React.useState(false);
  const [error, setError] = React.useState("");
  const request = React.useRef<AbortController | null>(null);
  React.useEffect(() => () => request.current?.abort(), []);
  const read = async (older = false) => {
    request.current?.abort();
    const controller = new AbortController();
    request.current = controller;
    setBusy(true); setError("");
    try {
      const result = await apiGetSessionPage(props.base, props.sessionId, {
        limit: 50, signal: controller.signal,
        ...(older && page ? { beforeIndex: page.page.next_before_index!, revision: page.page.revision } : {}),
      }, props.auth);
      if (!controller.signal.aborted) setPage(result);
    } catch (e) {
      if (controller.signal.aborted) return;
      // A changed generation invalidates even the currently displayed page.
      if (e instanceof SessionPageError && (e.status === 409 || e.status === 404)) setPage(null);
      setError(e instanceof SessionPageError && e.status === 409
        ? "Context changed. Load latest to start a new snapshot."
        : e instanceof Error ? e.message : String(e));
    } finally { if (!controller.signal.aborted) setBusy(false); }
  };
  return <details className="conversation-details" data-testid="session-context-pages"
    onToggle={event => {
      if (event.currentTarget.open && !opened) { setOpened(true); void read(); }
    }}>
    <summary>Model context snapshot</summary>
    <p>This is the current model context, not an append-only conversation archive. Compaction can replace it.
      Only one page (up to 50 messages / 2 MiB) is retained here.</p>
    <button type="button" disabled={busy} onClick={() => void read()}>Load latest context</button>
    <button type="button" disabled={busy || !page?.page.has_more} onClick={() => void read(true)}>Older context</button>
    {busy ? <p role="status">Loading context…</p> : null}
    {error ? <p role="alert">{error}</p> : null}
    {page ? <div>
      <p>Snapshot {page.page.revision} · {page.messages.length} messages{page.page.has_more ? " · older available" : " · beginning reached"}</p>
      {page.messages.map(message => <article key={message.index} className="mt-3">
        <strong>{message.role} · index {message.index}</strong>
        <pre className="whitespace-pre-wrap break-words">{message.content}</pre>
        {message.mm_json ? <details><summary>Media metadata</summary><pre className="whitespace-pre-wrap break-words">{message.mm_json}</pre></details> : null}
      </article>)}
    </div> : null}
  </details>;
}
