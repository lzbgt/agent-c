import React from "react";
import type { BrokerConnector } from "../../api";

function fmtTs(ms?: number | null) {
  if (!ms || !Number.isFinite(ms)) return "";
  try {
    return new Date(ms).toLocaleString();
  } catch {
    return String(ms);
  }
}

type BrokerConnectorsSectionProps = {
  canQuery: boolean;
  isFetching: boolean;
  error: unknown;
  connectors: BrokerConnector[];
  connectorStaleMinutes: string;
  setConnectorStaleMinutes: (next: string) => void;
  connectorStaleMs: number;
  onRefresh: () => void;
  onDownloadJson: () => void;
  onCopyJson: () => void;
};

export default function BrokerConnectorsSection(props: BrokerConnectorsSectionProps) {
  const {
    canQuery,
    isFetching,
    error,
    connectors,
    connectorStaleMinutes,
    setConnectorStaleMinutes,
    connectorStaleMs,
    onRefresh,
    onDownloadJson,
    onCopyJson,
  } = props;

  return (
    <section className="rounded-md border border-white/10 bg-black/20 p-3">
      <div className="mb-2 flex items-center justify-between gap-2">
        <div className="text-xs font-semibold text-white/80">Connector registry</div>
        <button
          className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
          type="button"
          disabled={!canQuery || isFetching}
          onClick={onRefresh}
        >
          {isFetching ? "Loading…" : "Refresh"}
        </button>
      </div>
      <div className="mb-2 flex flex-wrap items-center gap-2 text-[11px] text-white/60">
        <span>Stale after</span>
        <input
          className="w-16 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px]"
          value={connectorStaleMinutes}
          onChange={(e) => setConnectorStaleMinutes(e.target.value)}
          inputMode="numeric"
        />
        <span>minutes (local)</span>
      </div>

      {error ? (
        <div className="mb-2 rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
          {String(error)}
        </div>
      ) : null}

      {connectors.length === 0 ? (
        <div className="text-[11px] text-white/50">No connectors registered.</div>
      ) : (
        <div className="grid gap-2">
          <div className="flex flex-wrap items-center justify-between gap-2 text-[11px] text-white/60">
            <div>Showing {connectors.length} connectors.</div>
            <div className="flex flex-wrap items-center gap-2">
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[10px] text-white/70 hover:bg-black/40"
                type="button"
                onClick={onCopyJson}
              >
                Copy JSON
              </button>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[10px] text-white/70 hover:bg-black/40"
                type="button"
                onClick={onDownloadJson}
              >
                Download JSON
              </button>
            </div>
          </div>
          {connectors.map((connector) => {
            const id = String(connector?.id || "");
            const kind = String(connector?.kind || "");
            const status = String(connector?.status || "");
            const description = String(connector?.description || "");
            const lastSeenMs = typeof connector?.last_seen_unix_ms === "number" ? connector.last_seen_unix_ms : 0;
            const lastSeen = lastSeenMs ? fmtTs(lastSeenMs) : "";
            const nowMs = Date.now();
            const ageMs = lastSeenMs > 0 ? Math.max(0, nowMs - lastSeenMs) : 0;
            const isStale = lastSeenMs > 0 && ageMs > connectorStaleMs;
            const isMissing = lastSeenMs === 0;
            const lastError = String(connector?.last_error || "");
            const statusTone = isMissing ? "text-amber-200" : isStale ? "text-amber-200" : "text-emerald-200";
            const statusLabel = isMissing ? "unknown" : isStale ? "stale" : "fresh";
            const curlSnippet = `curl -H "Authorization: Bearer $BROKER_ADMIN_TOKEN" -H "Content-Type: application/json" -d '{"status":"${status || "ready"}","last_error":"","ts_unix_ms":0}' $BROKER_BASE/v1/connectors/${encodeURIComponent(
              id || "connector",
            )}/status`;
            const tooltip = [
              id ? `id: ${id}` : null,
              kind ? `kind: ${kind}` : null,
              status ? `status: ${status}` : null,
              lastSeenMs ? `last_seen_ms: ${lastSeenMs}` : null,
              lastError ? `last_error: ${lastError}` : null,
            ]
              .filter(Boolean)
              .join("\n");
            return (
              <div
                key={id}
                className="flex flex-wrap items-start justify-between gap-2 rounded-md border border-white/5 bg-black/30 px-2 py-1"
              >
                <div className="flex flex-col">
                  <div className="text-xs text-white/90">{id || "(unnamed)"}</div>
                  <div className="text-[11px] text-white/50" title={tooltip}>
                    {kind ? `kind: ${kind}` : "kind: (unspecified)"}
                    {status ? ` · ${status}` : ""}
                    <span className={`ml-2 ${statusTone}`}>· {statusLabel}</span>
                  </div>
                  {lastSeen ? <div className="text-[11px] text-white/50">last seen: {lastSeen}</div> : null}
                  {lastError ? <div className="mt-1 text-[11px] text-rose-200">last error: {lastError}</div> : null}
                  {description ? <div className="mt-1 text-[11px] text-white/60">{description}</div> : null}
                </div>
                <div className="flex items-center gap-2">
                  <button
                    className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[10px] text-white/70 hover:bg-black/40"
                    type="button"
                    title="Copy curl command"
                    onClick={() => {
                      try {
                        void navigator.clipboard.writeText(curlSnippet);
                      } catch {
                        // ignore clipboard errors
                      }
                    }}
                  >
                    Copy curl
                  </button>
                </div>
              </div>
            );
          })}
        </div>
      )}
    </section>
  );
}
