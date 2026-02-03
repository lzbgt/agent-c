import React from "react";
import EventTimeline from "./EventTimeline";
import Markdown from "./Markdown";
import type { AgentEvent, AgentdTraceResp, BrokerTraceResp } from "../api";

function safeNum(v: any): number {
  return typeof v === "number" && Number.isFinite(v) ? v : 0;
}

function safeStr(v: any): string {
  return typeof v === "string" ? v : "";
}

function fmtTs(ts: number): string {
  if (!ts) return "";
  try {
    return new Date(ts).toLocaleString();
  } catch {
    return String(ts);
  }
}

type TimelineItem =
  | { kind: "broker_relay"; ts: number; row: any }
  | { kind: "broker_orchestrate"; ts: number; row: any }
  | { kind: "agentd_record"; ts: number; agentId?: string; record: any }
  | { kind: "agentd_error"; ts: number; agentId?: string; row: any };

function buildTimelineFromAgentd(trace: AgentdTraceResp): TimelineItem[] {
  const out: TimelineItem[] = [];
  const recs = Array.isArray((trace as any)?.records) ? ((trace as any).records as any[]) : [];
  for (const r of recs) {
    const ts = safeNum(r?.ts_unix_ms);
    out.push({ kind: "agentd_record", ts, record: r });
  }
  return out;
}

function buildTimelineFromBroker(trace: BrokerTraceResp): TimelineItem[] {
  const out: TimelineItem[] = [];

  const orch = Array.isArray((trace as any)?.orchestrate) ? ((trace as any).orchestrate as any[]) : [];
  for (const row of orch) {
    out.push({ kind: "broker_orchestrate", ts: safeNum(row?.ts_unix_ms), row });
  }

  const relay = Array.isArray((trace as any)?.relay_audit) ? ((trace as any).relay_audit as any[]) : [];
  for (const row of relay) {
    out.push({ kind: "broker_relay", ts: safeNum(row?.ts_unix_ms), row });
  }

  const agentd = Array.isArray((trace as any)?.agentd) ? ((trace as any).agentd as any[]) : [];
  for (const row of agentd) {
    const agentId = safeStr(row?.agent_id);
    const body = row?.body;
    if (body && typeof body === "object" && Array.isArray((body as any).records)) {
      for (const r of (body as any).records as any[]) {
        out.push({ kind: "agentd_record", ts: safeNum(r?.ts_unix_ms), agentId, record: r });
      }
      continue;
    }
    // Represent agent-level failures as timeline items too (use response time as best-effort ts).
    const ts = Date.now();
    out.push({ kind: "agentd_error", ts, agentId, row });
  }

  return out;
}

function AgentdRecordCard({
  baseUrl,
  yolo,
  agentId,
  record,
}: {
  baseUrl: string;
  yolo: boolean;
  agentId?: string;
  record: any;
}) {
  const ts = safeNum(record?.ts_unix_ms);
  const when = fmtTs(ts);
  const sid = safeStr(record?.session_id);
  const ok = typeof record?.ok === "boolean" ? (record.ok as boolean) : undefined;
  const traceId = safeStr(record?.trace_id);
  const prompt = safeStr(record?.prompt);
  const assistantText = safeStr(record?.assistant_text);
  const events: AgentEvent[] = Array.isArray(record?.events) ? (record.events as any) : [];

  const status = ok === true ? "ok" : ok === false ? "error" : "";
  const summary = prompt.trim().length > 0 ? prompt.trim().slice(0, 160) : "(no prompt)";

  return (
    <details className="rounded-lg border border-white/10 bg-white/5 px-3 py-2" open>
      <summary className="cursor-pointer select-none text-xs text-white/80">
        <div className="flex min-w-0 flex-wrap items-baseline gap-x-2 gap-y-1">
          <span className="shrink-0 text-white/60">{when}</span>
          {agentId ? (
            <span className="shrink-0 text-indigo-300">
              agent <code className="text-indigo-200/80">{agentId}</code>
            </span>
          ) : null}
          {status ? <span className={status === "ok" ? "text-emerald-300" : "text-rose-300"}>{status}</span> : null}
          {sid ? (
            <span className="shrink-0 text-white/60">
              session <code className="text-white/70">{sid}</code>
            </span>
          ) : null}
          {traceId ? (
            <span className="shrink-0 text-white/50">
              trace <code className="text-white/60">{traceId}</code>
            </span>
          ) : null}
          <span className="min-w-0 flex-1">{summary}</span>
          <span className="shrink-0 text-white/40">({events.length} events)</span>
        </div>
      </summary>
      <div className="mt-3 grid gap-3">
        {assistantText ? (
          <div className="rounded-md border border-white/10 bg-black/20 px-3 py-2">
            <div className="text-[11px] font-semibold text-white/60">Assistant</div>
            <div className="mt-2">
              <Markdown text={assistantText} />
            </div>
          </div>
        ) : null}
        {events.length > 0 ? <EventTimeline baseUrl={baseUrl} yolo={yolo} events={events} /> : null}
      </div>
    </details>
  );
}

function BrokerRelayCard({ row }: { row: any }) {
  const ts = safeNum(row?.ts_unix_ms);
  const when = fmtTs(ts);
  const agentId = safeStr(row?.agent_id);
  const method = safeStr(row?.method);
  const path = safeStr(row?.path);
  const status = safeNum(row?.status);
  const latency = safeNum(row?.latency_ms);
  const err = safeStr(row?.error);
  const ok = status >= 200 && status < 300;

  return (
    <details className="rounded-lg border border-white/10 bg-white/5 px-3 py-2" open>
      <summary className="cursor-pointer select-none text-xs text-white/80">
        <div className="flex min-w-0 flex-wrap items-baseline gap-x-2 gap-y-1">
          <span className="shrink-0 text-white/60">{when}</span>
          <span className="shrink-0 text-sky-300">broker relay</span>
          {agentId ? (
            <span className="shrink-0 text-indigo-300">
              agent <code className="text-indigo-200/80">{agentId}</code>
            </span>
          ) : null}
          <span className={ok ? "shrink-0 text-emerald-300" : "shrink-0 text-rose-300"}>{status || ""}</span>
          <span className="shrink-0 text-white/50">{latency ? `${latency}ms` : ""}</span>
          <span className="min-w-0 flex-1">
            <code className="text-white/70">{method}</code> <code className="text-white/70">{path}</code>
          </span>
        </div>
      </summary>
      {err ? (
        <div className="mt-2 rounded-md border border-rose-500/30 bg-rose-500/10 px-3 py-2 text-xs text-rose-200">
          {err}
        </div>
      ) : null}
    </details>
  );
}

function BrokerOrchestrateCard({ row }: { row: any }) {
  const ts = safeNum(row?.ts_unix_ms);
  const when = fmtTs(ts);
  const traceId = safeStr(row?.trace_id);
  const req = row?.request_json;
  const resp = row?.response_json;

  const allOk = typeof resp?.all_ok === "boolean" ? (resp.all_ok as boolean) : undefined;
  const tasksTotal = safeNum(resp?.tasks_total);
  const results = Array.isArray(resp?.results) ? (resp.results as any[]) : [];

  const summary = (() => {
    if (tasksTotal > 0) return `${tasksTotal} tasks`;
    if (results.length > 0) return `${results.length} results`;
    return "orchestrate";
  })();

  return (
    <details className="rounded-lg border border-white/10 bg-white/5 px-3 py-2" open>
      <summary className="cursor-pointer select-none text-xs text-white/80">
        <div className="flex min-w-0 flex-wrap items-baseline gap-x-2 gap-y-1">
          <span className="shrink-0 text-white/60">{when}</span>
          <span className="shrink-0 text-fuchsia-300">broker orchestrate</span>
          {typeof allOk === "boolean" ? (
            <span className={allOk ? "shrink-0 text-emerald-300" : "shrink-0 text-rose-300"}>{allOk ? "all_ok" : "partial"}</span>
          ) : null}
          {traceId ? (
            <span className="shrink-0 text-white/50">
              trace <code className="text-white/60">{traceId}</code>
            </span>
          ) : null}
          <span className="min-w-0 flex-1 text-white/70">{summary}</span>
        </div>
      </summary>
      <div className="mt-3 grid gap-3">
        {req && typeof req === "object" ? (
          <div className="rounded-md border border-white/10 bg-black/20 px-3 py-2">
            <div className="text-[11px] font-semibold text-white/60">Request</div>
            <pre className="mt-2 overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/30 p-3 text-xs leading-relaxed text-white/90">
              {JSON.stringify(req, null, 2)}
            </pre>
          </div>
        ) : null}

        {resp && typeof resp === "object" ? (
          <div className="rounded-md border border-white/10 bg-black/20 px-3 py-2">
            <div className="text-[11px] font-semibold text-white/60">Response</div>
            <pre className="mt-2 overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/30 p-3 text-xs leading-relaxed text-white/90">
              {JSON.stringify(resp, null, 2)}
            </pre>
          </div>
        ) : null}
      </div>
    </details>
  );
}

function AgentdErrorCard({ row, agentId }: { row: any; agentId?: string }) {
  const ok = typeof row?.ok === "boolean" ? (row.ok as boolean) : false;
  const ms = safeNum(row?.ms);
  const err = safeStr(row?.error) || safeStr(row?.body?.error);
  const httpStatus = safeNum(row?.http_status) || safeNum(row?.agent_status);

  return (
    <div className="rounded-lg border border-white/10 bg-white/5 px-3 py-2 text-xs text-white/80">
      <div className="flex flex-wrap items-baseline gap-x-2 gap-y-1">
        <span className="shrink-0 text-white/60">{fmtTs(Date.now())}</span>
        <span className="shrink-0 text-white/70">agentd trace fetch</span>
        {agentId ? (
          <span className="shrink-0 text-indigo-300">
            agent <code className="text-indigo-200/80">{agentId}</code>
          </span>
        ) : null}
        <span className={ok ? "shrink-0 text-emerald-300" : "shrink-0 text-rose-300"}>{ok ? "ok" : "error"}</span>
        {httpStatus ? <span className="shrink-0 text-white/50">http {httpStatus}</span> : null}
        {ms ? <span className="shrink-0 text-white/50">{ms}ms</span> : null}
      </div>
      {err ? (
        <div className="mt-2 rounded-md border border-rose-500/30 bg-rose-500/10 px-3 py-2 text-xs text-rose-200">
          {err}
        </div>
      ) : null}
    </div>
  );
}

export default function TraceIdTimelineView({
  mode,
  baseUrl,
  yolo,
  traceId,
  agentdTrace,
  brokerTrace,
}: {
  mode: "direct" | "broker";
  baseUrl: string;
  yolo: boolean;
  traceId: string;
  agentdTrace?: AgentdTraceResp | null;
  brokerTrace?: BrokerTraceResp | null;
}) {
  const items: TimelineItem[] = React.useMemo(() => {
    if (mode === "broker") {
      if (!brokerTrace || brokerTrace.ok !== true) return [];
      const out = buildTimelineFromBroker(brokerTrace);
      out.sort((a, b) => (b.ts || 0) - (a.ts || 0));
      return out;
    }
    if (!agentdTrace || agentdTrace.ok !== true) return [];
    const out = buildTimelineFromAgentd(agentdTrace);
    out.sort((a, b) => (b.ts || 0) - (a.ts || 0));
    return out;
  }, [agentdTrace, brokerTrace, mode]);

  return (
    <div className="grid gap-2">
      <div className="text-xs text-white/60">
        trace <code className="text-white/80">{traceId}</code> · {items.length} items
      </div>
      {items.length === 0 ? (
        <div className="rounded-md border border-white/10 bg-black/20 px-3 py-3 text-xs text-white/60">
          No records found for this trace_id (or the run used <code className="font-mono">no_session=true</code> so it was not persisted).
        </div>
      ) : (
        <div className="grid gap-2">
          {items.map((it, idx) => {
            if (it.kind === "broker_orchestrate") {
              return <BrokerOrchestrateCard key={`orch-${idx}`} row={it.row} />;
            }
            if (it.kind === "broker_relay") {
              return <BrokerRelayCard key={`relay-${idx}`} row={it.row} />;
            }
            if (it.kind === "agentd_error") {
              return <AgentdErrorCard key={`agentd_err-${idx}`} agentId={it.agentId} row={it.row} />;
            }
            return <AgentdRecordCard key={`agentd-${idx}`} baseUrl={baseUrl} yolo={yolo} agentId={it.agentId} record={it.record} />;
          })}
        </div>
      )}
    </div>
  );
}
