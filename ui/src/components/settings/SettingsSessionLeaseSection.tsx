import FieldLabel from "../FieldLabel";
import SettingsBrokerSessionOperatorsSection from "./SettingsBrokerSessionOperatorsSection";
import type { SettingsConnectionSectionProps } from "./settingsConnectionTypes";

type SettingsSessionLeaseSectionProps = Pick<
  SettingsConnectionSectionProps,
  "connection" | "client" | "session"
>;

export default function SettingsSessionLeaseSection(props: SettingsSessionLeaseSectionProps) {
  const { connection, client, session } = props;
  const attachment = session.info?.attachment;
  const holderClientId = typeof attachment?.client_id === "string" ? attachment.client_id.trim() : "";
  const currentClientId = String(client.clientId || "").trim();
  const leaseActive = attachment?.lease_active === true;
  const leaseRole = leaseActive ? (holderClientId && currentClientId && holderClientId === currentClientId ? "owner" : "observer") : "unleased";
  const leaseExpiresLabel =
    typeof attachment?.lease_expires_at_ms === "number" && Number.isFinite(attachment.lease_expires_at_ms)
      ? new Date(attachment.lease_expires_at_ms).toLocaleString()
      : "";
  const streamLastEventLabel =
    typeof session.streamLastEventAtMs === "number" && Number.isFinite(session.streamLastEventAtMs)
      ? new Date(session.streamLastEventAtMs).toLocaleString()
      : "";
  const streamUpdatedLabel =
    typeof session.streamUpdatedMs === "number" && Number.isFinite(session.streamUpdatedMs)
      ? new Date(session.streamUpdatedMs).toLocaleString()
      : "";
  const sessionBusy = session.attachPending || session.renewPending || session.releasePending;

  return (
    <div className="col-span-2" data-testid="settings-session-lease-section">
      <FieldLabel>Session</FieldLabel>
      <input
        className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
        data-testid="session-id-input"
        value={session.id}
        onChange={(e) => session.setId(e.target.value)}
      />
      <div className="mt-2 grid grid-cols-2 gap-3">
        <div>
          <FieldLabel>Lease seconds</FieldLabel>
          <input
            className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
            data-testid="session-lease-seconds-input"
            value={session.leaseSeconds}
            onChange={(e) => session.setLeaseSeconds(e.target.value)}
            inputMode="numeric"
            placeholder="90"
          />
        </div>
        <div className="rounded-md border border-white/10 bg-black/20 px-3 py-2 text-[11px] text-white/70">
          <div className="font-semibold text-white/80">Lease status</div>
          <div className="mt-1">
            {leaseRole === "owner"
              ? "Owner: this client currently holds the attachment lease."
              : leaseRole === "observer"
                ? `Observer: ${holderClientId || "another client"} currently holds the active lease.`
                : "No active lease reported for this session."}
          </div>
          {holderClientId ? (
            <div className="mt-1">
              holder: <code className="font-mono text-white/80">{holderClientId}</code>
            </div>
          ) : null}
          {attachment?.lease_seconds ? <div className="mt-1">lease_seconds: {attachment.lease_seconds}</div> : null}
          {leaseExpiresLabel ? <div className="mt-1">expires: {leaseExpiresLabel}</div> : null}
          {session.info?.thread_id ? (
            <div className="mt-1">
              thread: <code className="font-mono text-white/80">{session.info.thread_id}</code>
            </div>
          ) : null}
        </div>
      </div>
      <div className="mt-2 flex flex-wrap items-center gap-2 text-[11px]">
        <button
          className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-white/80 hover:bg-black/40 disabled:opacity-50"
          type="button"
          onClick={() => session.attach()}
          disabled={!String(session.id || "").trim() || sessionBusy}
        >
          {session.attachPending ? "Attaching…" : "Attach / claim"}
        </button>
        <button
          className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-white/80 hover:bg-black/40 disabled:opacity-50"
          type="button"
          onClick={() => session.renewAttachment()}
          disabled={!String(session.id || "").trim() || sessionBusy}
        >
          {session.renewPending ? "Renewing…" : "Renew lease"}
        </button>
        <button
          className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-white/80 hover:bg-black/40 disabled:opacity-50"
          type="button"
          onClick={() => session.releaseAttachment()}
          disabled={!String(session.id || "").trim() || sessionBusy}
        >
          {session.releasePending ? "Releasing…" : "Release lease"}
        </button>
        {session.leaseConflict ? (
          <button
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-white/70 hover:bg-black/40"
            type="button"
            onClick={() => session.clearLeaseConflict()}
          >
            Clear conflict
          </button>
        ) : null}
      </div>
      <div className="mt-2 text-[11px] text-white/60">
        In broker mode, these controls use the broker session attachment surface so owner/observer/rival conflicts stay explicit.
      </div>
      <div className="mt-2 rounded-md border border-white/10 bg-black/20 px-3 py-2 text-[11px] text-white/70" data-testid="session-stream-status">
        <div className="font-semibold text-white/80">Session event stream</div>
        <div className="mt-1">
          status: <span className="font-mono text-white/80">{session.streamStatus}</span>
        </div>
        {session.streamLastEventId ? (
          <div className="mt-1">
            last_event_id: <code className="font-mono text-white/80">{session.streamLastEventId}</code>
          </div>
        ) : null}
        <div className="mt-1">buffered_events: {session.streamBufferedCount}</div>
        {streamLastEventLabel ? <div className="mt-1">last_event_at: {streamLastEventLabel}</div> : null}
        {streamUpdatedLabel ? <div className="mt-1">persisted_at: {streamUpdatedLabel}</div> : null}
        {session.streamError ? <div className="mt-1 text-rose-200">stream error: {session.streamError}</div> : null}
        <div className="mt-1 text-white/50">
          Broker session replay uses <code className="font-mono">Last-Event-ID</code> and persists a bounded event buffer locally.
        </div>
      </div>
      {session.attachError ? <div className="mt-2 text-[11px] text-rose-200">Attach failed: {session.attachError}</div> : null}
      {session.renewError ? <div className="mt-2 text-[11px] text-rose-200">Renew failed: {session.renewError}</div> : null}
      {session.releaseError ? <div className="mt-2 text-[11px] text-rose-200">Release failed: {session.releaseError}</div> : null}
      {session.leaseConflict ? (
        <div className="mt-2 rounded-md border border-amber-500/30 bg-amber-500/10 px-3 py-2 text-[11px] text-amber-100">
          <div className="font-semibold">Lease conflict</div>
          <div className="mt-1">{session.leaseConflict.message}</div>
          {session.leaseConflict.requestedClientId ? (
            <div className="mt-1">
              requested client: <code className="font-mono">{session.leaseConflict.requestedClientId}</code>
            </div>
          ) : null}
          {session.leaseConflict.currentAttachment?.client_id ? (
            <div className="mt-1">
              current holder: <code className="font-mono">{session.leaseConflict.currentAttachment.client_id}</code>
            </div>
          ) : null}
        </div>
      ) : null}
      <div className="mt-4">
        <SettingsBrokerSessionOperatorsSection connection={connection} client={client} session={session} />
      </div>
    </div>
  );
}
