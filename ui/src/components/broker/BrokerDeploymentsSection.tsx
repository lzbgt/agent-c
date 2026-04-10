import React from "react";
import type { BrokerDeploymentInfo } from "../../api";
import FieldLabel from "../FieldLabel";
import { asFiniteNumber, normalizeDeploymentId, type BrokerFanoutResult } from "./brokerPanelUtils";
import { fmtTs } from "./useBrokerPanelState";

type BrokerDeploymentsSectionProps = {
  canQuery: boolean;
  agentId: string;
  isFetching: boolean;
  error: unknown;
  deployments: BrokerDeploymentInfo[];
  defaultDeploymentId: string;
  selectedDeploymentSet: Set<string>;
  selectedDeployments: string[];
  toggleDeployment: (id: string) => void;
  selectConnectedDeployments: (deployments: BrokerDeploymentInfo[]) => void;
  selectAllDeployments: (deployments: BrokerDeploymentInfo[]) => void;
  onRefresh: () => void;
  otaUrl: string;
  setOtaUrl: (value: string) => void;
  otaSha256: string;
  setOtaSha256: (value: string) => void;
  otaVersion: string;
  setOtaVersion: (value: string) => void;
  otaDrainMs: string;
  setOtaDrainMs: (value: string) => void;
  otaReason: string;
  setOtaReason: (value: string) => void;
  otaBusy: boolean;
  otaError: string | null;
  otaResults: BrokerFanoutResult[] | null;
  onRunOtaUpdate: () => void;
  otaStatusBusy: boolean;
  otaStatusError: string | null;
  otaStatusResults: BrokerFanoutResult[] | null;
  otaStatusCachedAt: string | null;
  onRunOtaStatus: () => void;
  onClearOtaStatusCache: () => void;
};

export default function BrokerDeploymentsSection(props: BrokerDeploymentsSectionProps) {
  const {
    canQuery,
    agentId,
    isFetching,
    error,
    deployments,
    defaultDeploymentId,
    selectedDeploymentSet,
    selectedDeployments,
    toggleDeployment,
    selectConnectedDeployments,
    selectAllDeployments,
    onRefresh,
    otaUrl,
    setOtaUrl,
    otaSha256,
    setOtaSha256,
    otaVersion,
    setOtaVersion,
    otaDrainMs,
    setOtaDrainMs,
    otaReason,
    setOtaReason,
    otaBusy,
    otaError,
    otaResults,
    onRunOtaUpdate,
    otaStatusBusy,
    otaStatusError,
    otaStatusResults,
    otaStatusCachedAt,
    onRunOtaStatus,
    onClearOtaStatusCache,
  } = props;

  return (
    <section className="rounded-md border border-white/10 bg-black/20 p-3">
      <div className="mb-2 flex items-center justify-between gap-2">
        <div className="text-xs font-semibold text-white/80">Deployments + OTA</div>
        <button
          className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
          type="button"
          disabled={!canQuery || !agentId || isFetching}
          onClick={onRefresh}
        >
          {isFetching ? "Loading…" : "Refresh"}
        </button>
      </div>

      {!agentId ? (
        <div className="text-[11px] text-white/50">Select an agent to manage deployments.</div>
      ) : error ? (
        <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
          {String(error)}
        </div>
      ) : deployments.length === 0 ? (
        <div className="text-[11px] text-white/50">No deployments connected.</div>
      ) : (
        <>
          {defaultDeploymentId ? (
            <div className="mb-2 text-[11px] text-white/50">
              Broker default: <span className="font-mono text-white/80">{defaultDeploymentId}</span>
            </div>
          ) : null}
          <div className="grid gap-2">
            {deployments.map((dep) => {
              const id = normalizeDeploymentId(dep.deployment_id);
              const selected = selectedDeploymentSet.has(id);
              const connected = dep.connected === true;
              return (
                <label
                  key={id}
                  className="flex flex-wrap items-center justify-between gap-2 rounded-md border border-white/5 bg-black/30 px-2 py-1"
                >
                  <div className="flex items-center gap-2">
                    <input type="checkbox" checked={selected} onChange={() => toggleDeployment(id)} />
                    <div className="flex flex-col">
                      <div className="text-xs text-white/90">{id}</div>
                      <div className="text-[11px] text-white/50">{connected ? "connected" : "disconnected"}</div>
                    </div>
                  </div>
                  <button
                    className={
                      selected
                        ? "rounded-md border border-emerald-400/40 bg-emerald-500/10 px-2 py-1 text-[11px] text-emerald-100"
                        : "rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                    }
                    type="button"
                    onClick={() => toggleDeployment(id)}
                  >
                    {selected ? "Selected" : "Select"}
                  </button>
                </label>
              );
            })}
          </div>
          <div className="mt-2 flex flex-wrap items-center gap-2">
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
              type="button"
              onClick={() => selectConnectedDeployments(deployments)}
            >
              Select connected
            </button>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
              type="button"
              onClick={() => selectAllDeployments(deployments)}
            >
              Select all
            </button>
          </div>
        </>
      )}

      <div className="mt-3 grid gap-2">
        <FieldLabel>OTA update</FieldLabel>
        <input
          className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40"
          placeholder="https://.../agentd.tar.gz"
          value={otaUrl}
          onChange={(e) => setOtaUrl(e.target.value)}
        />
        <div className="grid grid-cols-1 gap-2 md:grid-cols-2">
          <input
            className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40"
            placeholder="sha256 (optional)"
            value={otaSha256}
            onChange={(e) => setOtaSha256(e.target.value)}
          />
          <input
            className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40"
            placeholder="version label (optional)"
            value={otaVersion}
            onChange={(e) => setOtaVersion(e.target.value)}
          />
        </div>
        <div className="grid grid-cols-1 gap-2 md:grid-cols-2">
          <input
            className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40"
            placeholder="drain timeout ms (default 15000)"
            value={otaDrainMs}
            onChange={(e) => setOtaDrainMs(e.target.value)}
          />
          <input
            className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40"
            placeholder="reason (optional)"
            value={otaReason}
            onChange={(e) => setOtaReason(e.target.value)}
          />
        </div>
        <button
          className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50"
          type="button"
          disabled={!agentId || otaBusy || selectedDeployments.length === 0}
          onClick={onRunOtaUpdate}
        >
          {otaBusy ? "Updating…" : "Run OTA update"}
        </button>
        {otaError ? (
          <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
            {otaError}
          </div>
        ) : null}
        {otaResults && otaResults.length > 0 ? (
          <div className="grid gap-2">
            {otaResults.map((row) => {
              const depId = String(row?.deployment_id || "");
              const status = row?.status;
              const ok = row?.data?.ok === true;
              const err = row?.data?.error || row?.data?.err;
              const respStatus = row?.data?.status || "";
              return (
                <div
                  key={`ota-${depId}`}
                  className="rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70"
                >
                  <div className="text-xs text-white/90">
                    {depId} · {ok ? "ok" : "error"} · http {status}
                  </div>
                  <div className="text-[11px] text-white/50">
                    {respStatus ? `status ${respStatus}` : "no status"}
                    {err ? ` · ${String(err)}` : ""}
                  </div>
                </div>
              );
            })}
          </div>
        ) : null}

        <div className="mt-3 grid gap-2">
          <FieldLabel>OTA status</FieldLabel>
          {otaStatusCachedAt ? (
            <div className="flex items-center justify-between text-[11px] text-white/50">
              <span>Cached: {otaStatusCachedAt} (ttl 10m)</span>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[10px] text-white/60 hover:bg-black/40"
                type="button"
                onClick={onClearOtaStatusCache}
              >
                Clear
              </button>
            </div>
          ) : null}
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={!agentId || otaStatusBusy || selectedDeployments.length === 0}
            onClick={onRunOtaStatus}
          >
            {otaStatusBusy ? "Checking…" : "Fetch OTA status"}
          </button>
          {otaStatusError ? (
            <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
              {otaStatusError}
            </div>
          ) : null}
          {otaStatusResults && otaStatusResults.length > 0 ? (
            <div className="grid gap-2">
              {otaStatusResults.map((row) => {
                const depId = String(row?.deployment_id || "");
                const status = row?.status;
                const ok = row?.data?.ok === true;
                const planStatus = row?.data?.status || "unknown";
                const updated = fmtTs(asFiniteNumber(row?.data?.updated_unix_ms));
                const drainActive = row?.data?.drain_active === true;
                const drainUntil = fmtTs(asFiniteNumber(row?.data?.drain_until_unix_ms));
                const drainReason = row?.data?.drain_reason;
                const otaId = row?.data?.ota_id;
                const jobsRunning = row?.data?.jobs_running;
                const jobsQueued = row?.data?.jobs_queued;
                const workflowsRunning = row?.data?.workflows_running;
                const wfTasksRunning = row?.data?.workflow_tasks_running;
                const wfTasksQueued = row?.data?.workflow_tasks_queued;
                const err = row?.data?.last_error || row?.data?.error || row?.data?.err;
                const inflightParts: string[] = [];
                if (typeof jobsRunning === "number") inflightParts.push(`jobs ${jobsRunning}`);
                if (typeof workflowsRunning === "number") inflightParts.push(`workflows ${workflowsRunning}`);
                if (typeof wfTasksRunning === "number") inflightParts.push(`tasks ${wfTasksRunning}`);
                const queuedParts: string[] = [];
                if (typeof jobsQueued === "number") queuedParts.push(`jobs ${jobsQueued}`);
                if (typeof wfTasksQueued === "number") queuedParts.push(`tasks ${wfTasksQueued}`);
                return (
                  <div
                    key={`ota-status-${depId}`}
                    className="rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70"
                  >
                    <div className="text-xs text-white/90">
                      {depId} · {ok ? "ok" : "error"} · http {status}
                    </div>
                    <div className="text-[11px] text-white/50">
                      {planStatus ? `status ${planStatus}` : "no status"}
                      {otaId ? ` · ota ${String(otaId)}` : ""}
                      {updated ? ` · updated ${updated}` : ""}
                      {drainActive ? " · draining" : " · idle"}
                      {drainUntil ? ` (until ${drainUntil})` : ""}
                      {drainReason ? ` · reason ${String(drainReason)}` : ""}
                      {inflightParts.length > 0 ? ` · running ${inflightParts.join(", ")}` : ""}
                      {queuedParts.length > 0 ? ` · queued ${queuedParts.join(", ")}` : ""}
                      {err ? ` · ${String(err)}` : ""}
                    </div>
                  </div>
                );
              })}
            </div>
          ) : null}
        </div>
      </div>
    </section>
  );
}
