import React from "react";
import type {
  AgentEvent,
  AgentdTraceRecord,
  AgentdTraceResp,
  BrokerTraceAgentdFanoutRow,
  BrokerTraceOrchestrateRow,
  BrokerTraceRelayAuditRow,
  BrokerTraceResp,
} from "../api";
import { safeObject } from "../jsonUtils";
import EventTimeline from "./EventTimeline";
import Markdown from "./Markdown";

function safeNum(value: unknown): number {
  return typeof value === "number" && Number.isFinite(value) ? value : 0;
}

function safeStr(value: unknown): string {
  return typeof value === "string" ? value : "";
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
  | { kind: "broker_relay"; ts: number; row: BrokerTraceRelayAuditRow }
  | { kind: "broker_orchestrate"; ts: number; row: BrokerTraceOrchestrateRow }
  | { kind: "agentd_record"; ts: number; agentId?: string; record: AgentdTraceRecord }
  | { kind: "agentd_error"; ts: number; agentId?: string; row: BrokerTraceAgentdFanoutRow };

function buildTimelineFromAgentd(trace: AgentdTraceResp): TimelineItem[] {
  return (trace.records ?? []).map((record) => ({
    kind: "agentd_record" as const,
    ts: safeNum(record.ts_unix_ms),
    record,
  }));
}

function buildTimelineFromBroker(trace: BrokerTraceResp): TimelineItem[] {
  const items: TimelineItem[] = [];

  for (const row of trace.orchestrate ?? []) {
    items.push({ kind: "broker_orchestrate", ts: safeNum(row.ts_unix_ms), row });
  }

  for (const row of trace.relay_audit ?? []) {
    items.push({ kind: "broker_relay", ts: safeNum(row.ts_unix_ms), row });
  }

  for (const row of trace.agentd ?? []) {
    const agentId = safeStr(row.agent_id);
    const records = row.body?.records ?? [];
    if (records.length > 0) {
      for (const record of records) {
        items.push({
          kind: "agentd_record",
          ts: safeNum(record.ts_unix_ms),
          agentId,
          record,
        });
      }
      continue;
    }

    items.push({
      kind: "agentd_error",
      ts: Date.now(),
      agentId,
      row,
    });
  }

  return items;
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
  record: AgentdTraceRecord;
}) {
  const ts = safeNum(record.ts_unix_ms);
  const when = fmtTs(ts);
  const sid = safeStr(record.session_id);
  const ok = typeof record.ok === "boolean" ? record.ok : undefined;
  const traceId = safeStr(record.trace_id);
  const prompt = safeStr(record.prompt);
  const assistantText = safeStr(record.assistant_text);
  const events: AgentEvent[] = Array.isArray(record.events) ? record.events : [];

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

function BrokerRelayCard({ row }: { row: BrokerTraceRelayAuditRow }) {
  const ts = safeNum(row.ts_unix_ms);
  const when = fmtTs(ts);
  const agentId = safeStr(row.agent_id);
  const method = safeStr(row.method);
  const path = safeStr(row.path);
  const status = safeNum(row.status);
  const latency = safeNum(row.latency_ms);
  const err = safeStr(row.error);
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

function BrokerOrchestrateCard({ row }: { row: BrokerTraceOrchestrateRow }) {
  const ts = safeNum(row.ts_unix_ms);
  const when = fmtTs(ts);
  const traceId = safeStr(row.trace_id);
  const req = row.request_json;
  const resp = row.response_json;
  const respRecord = safeObject(resp);

  const allOk = typeof respRecord.all_ok === "boolean" ? respRecord.all_ok : undefined;
  const tasksTotal = safeNum(respRecord.tasks_total);
  const results = Array.isArray(respRecord.results) ? respRecord.results : [];

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
        <div className="rounded-md border border-white/10 bg-black/20 px-3 py-2">
          <div className="text-[11px] font-semibold text-white/60">Request</div>
          <pre className="mt-2 overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/30 p-3 text-xs leading-relaxed text-white/90">
            {JSON.stringify(req, null, 2)}
          </pre>
        </div>

        <div className="rounded-md border border-white/10 bg-black/20 px-3 py-2">
          <div className="text-[11px] font-semibold text-white/60">Response</div>
          <pre className="mt-2 overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/30 p-3 text-xs leading-relaxed text-white/90">
            {JSON.stringify(resp, null, 2)}
          </pre>
        </div>
      </div>
    </details>
  );
}

function AgentdErrorCard({
  ts,
  row,
  agentId,
}: {
  ts: number;
  row: BrokerTraceAgentdFanoutRow;
  agentId?: string;
}) {
  const ok = typeof row.ok === "boolean" ? row.ok : false;
  const ms = safeNum(row.ms);
  const err = safeStr(row.error) || safeStr(row.body?.error);
  const httpStatus = safeNum(row.http_status) || safeNum(row.agent_status);

  return (
    <div className="rounded-lg border border-white/10 bg-white/5 px-3 py-2 text-xs text-white/80">
      <div className="flex flex-wrap items-baseline gap-x-2 gap-y-1">
        <span className="shrink-0 text-white/60">{fmtTs(ts)}</span>
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
      const timeline = buildTimelineFromBroker(brokerTrace);
      timeline.sort((a, b) => (b.ts || 0) - (a.ts || 0));
      return timeline;
    }
    if (!agentdTrace || agentdTrace.ok !== true) return [];
    const timeline = buildTimelineFromAgentd(agentdTrace);
    timeline.sort((a, b) => (b.ts || 0) - (a.ts || 0));
    return timeline;
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
          {items.map((item, index) => {
            if (item.kind === "broker_orchestrate") {
              return <BrokerOrchestrateCard key={`orch-${index}`} row={item.row} />;
            }
            if (item.kind === "broker_relay") {
              return <BrokerRelayCard key={`relay-${index}`} row={item.row} />;
            }
            if (item.kind === "agentd_error") {
              return <AgentdErrorCard key={`agentd_err-${index}`} ts={item.ts} agentId={item.agentId} row={item.row} />;
            }
            return (
              <AgentdRecordCard
                key={`agentd-${index}`}
                baseUrl={baseUrl}
                yolo={yolo}
                agentId={item.agentId}
                record={item.record}
              />
            );
          })}
        </div>
      )}
    </div>
  );
}
