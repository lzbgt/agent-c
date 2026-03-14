import React from "react";
import { SectionHeader } from "./SettingsControls";

type SessionSettingsProps = {
  session: {
    id: string;
    setId: (next: string) => void;
    sessions: string[];
    refresh: () => void;
    newSession: () => void;
    newSessionPending: boolean;
    deleteSession: (sid: string) => void;
    deletePending: boolean;
    deleteError: string | null;
    clearAll: () => void;
    clearAllPending: boolean;
    clearAllError: string | null;
  };
  clearAllArmed: boolean;
  setClearAllArmed: React.Dispatch<React.SetStateAction<boolean>>;
  clearAllArmTimeoutRef: React.MutableRefObject<number>;
};

export default function SettingsSessionsSection(props: SessionSettingsProps) {
  const { session, clearAllArmed, setClearAllArmed, clearAllArmTimeoutRef } = props;

  return (
    <div className="mt-4">
      <SectionHeader title="Sessions" />
      <div className="mt-2 flex flex-wrap gap-2">
        <button
          className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40"
          onClick={() => session.refresh()}
          type="button"
        >
          Refresh
        </button>
        <button
          className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50"
          onClick={() => session.newSession()}
          type="button"
          data-testid="new-session"
          disabled={session.newSessionPending}
        >
          New session
        </button>
        <button
          className="rounded-md border border-rose-500/30 bg-rose-500/10 px-3 py-2 text-xs text-rose-200 hover:bg-rose-500/15 disabled:opacity-50"
          onClick={() => {
            const ids = session.sessions ?? [];
            if (ids.length === 0) return;
            if (!clearAllArmed) {
              setClearAllArmed(true);
              try {
                if (clearAllArmTimeoutRef.current) window.clearTimeout(clearAllArmTimeoutRef.current);
              } catch {
                // ignore
              }
              clearAllArmTimeoutRef.current = window.setTimeout(() => setClearAllArmed(false), 8000);
              return;
            }
            setClearAllArmed(false);
            session.clearAll();
          }}
          type="button"
          disabled={session.clearAllPending}
          title="Danger: deletes all sessions on the daemon."
        >
          {clearAllArmed ? `Confirm clear all (${session.sessions.length})` : "Clear all"}
        </button>
        {clearAllArmed ? (
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40"
            type="button"
            onClick={() => setClearAllArmed(false)}
          >
            Cancel
          </button>
        ) : null}
      </div>
      <div className="mt-2 max-h-64 overflow-auto rounded-md border border-white/10 bg-black/20">
        {(session.sessions ?? []).map((sid) => {
          const selected = sid === session.id;
          return (
            <div
              key={sid}
              className={`flex items-center justify-between gap-2 px-3 py-2 text-xs ${selected ? "bg-white/10" : ""}`}
            >
              <button className="flex-1 text-left hover:underline" onClick={() => session.setId(sid)} type="button">
                {sid}
              </button>
              <button
                className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200 hover:bg-rose-500/15 disabled:opacity-50"
                type="button"
                disabled={session.deletePending}
                onClick={() => {
                  if (!confirm(`Delete session '${sid}'?`)) return;
                  session.deleteSession(sid);
                }}
              >
                Delete
              </button>
            </div>
          );
        })}
      </div>
      {session.deleteError ? (
        <div className="mt-2 rounded-md border border-rose-500/30 bg-rose-500/10 px-3 py-2 text-xs text-rose-200">
          Delete failed: {session.deleteError}
        </div>
      ) : null}
      {session.clearAllError ? (
        <div className="mt-2 rounded-md border border-rose-500/30 bg-rose-500/10 px-3 py-2 text-xs text-rose-200">
          Clear all failed: {session.clearAllError}
        </div>
      ) : null}
    </div>
  );
}
