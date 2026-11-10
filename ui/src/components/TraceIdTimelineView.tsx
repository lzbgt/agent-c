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
import { JsonDetails, ToolError } from "./ToolOutput";
import { executionEvents, executionMetadata } from "../history/executionEvidence";

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

function AgentdRecordCard({ baseUrl, yolo, agentId, record }: {
  baseUrl: string; yolo: boolean; agentId?: string; record: AgentdTraceRecord;
}) {
  const [activityOpen, setActivityOpen] = React.useState(false);
  const events: AgentEvent[] = executionEvents(Array.isArray(record.events) ? record.events : []);
  const error = safeStr(record.error) || safeStr(record.err) || (record.ok === false ? "This run failed." : "");
  return <article className="tool-card" data-testid="trace-output">
    {record.prompt ? <div><p className="tool-muted">You</p><Markdown text={record.prompt} /></div> : null}
    <div className="tool-preview"><p className="tool-muted">{agentId ? `Agent · ${agentId}` : "Agent"}</p>
      {record.assistant_text ? <Markdown text={record.assistant_text} /> : <p className="tool-muted">No assistant output recorded.</p>}
    </div>
    <ToolError error={error || null} />
    <JsonDetails label="Record details" value={executionMetadata({ ...record, events: undefined })} />
    {events.length ? <details className="tool-details" onToggle={(event) => setActivityOpen(event.currentTarget.open)}>
      <summary>Tool activity</summary>
      {activityOpen ? <EventTimeline baseUrl={baseUrl} yolo={yolo} events={events} /> : null}
    </details> : null}
  </article>;
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
    <details className="rounded-lg border border-white/10 bg-white/5 px-3 py-2">
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
    <details className="rounded-lg border border-white/10 bg-white/5 px-3 py-2">
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
            {JSON.stringify(executionMetadata(req), null, 2)}
          </pre>
        </div>

        <div className="rounded-md border border-white/10 bg-black/20 px-3 py-2">
          <div className="text-[11px] font-semibold text-white/60">Response</div>
          <pre className="mt-2 overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/30 p-3 text-xs leading-relaxed text-white/90">
            {JSON.stringify(executionMetadata(resp), null, 2)}
          </pre>
        </div>
      </div>
    </details>
  );
}

function AgentdErrorCard({ row, agentId }: { row: BrokerTraceAgentdFanoutRow; agentId?: string }) {
  const error = safeStr(row.error) || safeStr(row.body?.error);
  return <div className="tool-card">
    {row.ok === true && !error ? <p className="tool-muted">No saved records from {agentId || "this agent"}.</p> : <ToolError error={`${agentId || "Agent"}: ${error || "Trace lookup failed."}`} />}
    <JsonDetails label="Lookup details" value={row} />
  </div>;
}

export default function TraceIdTimelineView({
  mode,
  baseUrl,
  yolo,
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

  const records = items.filter((item) => item.kind === "agentd_record" || item.kind === "agentd_error");
  const transport = items.filter((item) => item.kind === "broker_relay" || item.kind === "broker_orchestrate");
  return <div className="grid gap-4" data-testid="trace-results">
    {records.length ? records.map((item, index) => item.kind === "agentd_error" ?
      <AgentdErrorCard key={`fetch-${index}`} agentId={item.agentId} row={item.row} /> : item.kind === "agentd_record" ?
      <AgentdRecordCard key={`record-${index}`} baseUrl={baseUrl} yolo={yolo} agentId={item.agentId} record={item.record} /> : null
    ) : <p className="tool-empty">No saved conversation records found for this trace.</p>}
    {transport.map((item, index) => {
      if (item.kind === "broker_relay" && (safeStr(item.row.error) || safeNum(item.row.status) >= 400)) return <ToolError key={index} error={safeStr(item.row.error) || `Relay returned HTTP ${item.row.status}.`} />;
      if (item.kind === "broker_orchestrate" && safeObject(item.row.response_json).all_ok === false) return <ToolError key={index} error="Some orchestrated tasks did not complete. Open transport details to inspect their results." />;
      return null;
    })}
    {transport.length ? <details className="tool-details tool-maintenance" data-testid="trace-transport-details">
      <summary>Transport & orchestration details</summary>
      {transport.map((item, index) => item.kind === "broker_relay" ? <BrokerRelayCard key={index} row={item.row} /> : item.kind === "broker_orchestrate" ? <BrokerOrchestrateCard key={index} row={item.row} /> : null)}
    </details> : null}
    <JsonDetails label="Trace response details" value={mode === "broker" ? { trace_id: brokerTrace?.trace_id, record_count: records.length, transport_count: transport.length } : { trace_id: agentdTrace?.trace_id, record_count: records.length }} />
  </div>;
}
