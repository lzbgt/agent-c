import React from "react";
import { type ApiAuth } from "../api";
import BrokerAgentsSection from "./broker/BrokerAgentsSection";
import BrokerAuditSection from "./broker/BrokerAuditSection";
import BrokerConnectorsSection from "./broker/BrokerConnectorsSection";
import BrokerDeploymentsSection from "./broker/BrokerDeploymentsSection";
import BrokerEventsSection from "./broker/BrokerEventsSection";
import BrokerMembersSection from "./broker/BrokerMembersSection";
import BrokerMemorySection from "./broker/BrokerMemorySection";
import BrokerTeamConsole from "./broker/BrokerTeamConsole";
import useBrokerPanelState from "./broker/useBrokerPanelState";

export type BrokerPanelProps = {
  open: boolean;
  onToggle: (open: boolean) => void;
  brokerBase: string;
  brokerAgentId: string;
  setBrokerAgentId: (next: string) => void;
  auth: ApiAuth;
  authKey: string;
  clientId: string;
};

export default function BrokerPanel(props: BrokerPanelProps) {
  const state = useBrokerPanelState({
    brokerBase: props.brokerBase,
    brokerAgentId: props.brokerAgentId,
    auth: props.auth,
    authKey: props.authKey,
    clientId: props.clientId,
    open: props.open,
  });

  return (
    <details
      className="mb-4 rounded-lg border border-white/10 bg-white/5 px-3 py-2"
      open={props.open}
      onToggle={(ev) => props.onToggle((ev.currentTarget as HTMLDetailsElement).open)}
    >
      <summary className="cursor-pointer select-none text-xs text-white/80">
        <div className="flex flex-wrap items-center justify-between gap-2">
          <div className="font-semibold text-white/80">Broker panel</div>
          <div className="text-[11px] text-white/50">Manage agents + membership when in broker mode</div>
        </div>
      </summary>

      <div className="mt-3 grid gap-4">
        {!state.base ? (
          <div className="rounded-md border border-amber-500/30 bg-amber-500/10 px-3 py-2 text-xs text-amber-100">
            Missing broker base URL. Set it in Settings.
          </div>
        ) : !state.useCookieAuth && String(props.auth?.token || "").trim().length === 0 ? (
          <div className="rounded-md border border-amber-500/30 bg-amber-500/10 px-3 py-2 text-xs text-amber-100">
            Missing broker auth token (OIDC). Set it in Settings.
          </div>
        ) : null}

        <div className="grid gap-3">
          <div className="rounded-md border border-white/10 bg-black/20 p-3">
            <div className="text-[11px] font-semibold text-white/60">Broker pages</div>
            <div className="mt-2 flex flex-wrap gap-2">
              {state.brokerPages.map((page) => {
                const active = page.id === state.brokerPage;
                return (
                  <button
                    key={page.id}
                    className={`rounded-md px-3 py-1.5 text-xs whitespace-nowrap ${
                      active ? "bg-indigo-500/20 text-indigo-100" : "bg-black/20 text-white/70 hover:bg-black/30"
                    }`}
                    type="button"
                    onClick={() => state.setBrokerPage(page.id)}
                  >
                    {page.label}
                  </button>
                );
              })}
            </div>
          </div>

          <div className="min-w-0 grid gap-4">
            {state.brokerPage === "teams" ? (
              <BrokerTeamConsole
                base={state.base}
                auth={props.auth}
                authKey={props.authKey}
                clientId={props.clientId}
                quorumEvents={state.brokerEventsState.brokerEvents}
              />
            ) : null}

            {state.brokerPage === "agents" ? (
              <BrokerAgentsSection
                canQuery={state.canQuery}
                isFetching={state.agentsQuery.isFetching}
                error={state.agentsQuery.error}
                agents={state.agents}
                agentId={state.agentId}
                onRefresh={() => void state.agentsQuery.refetch()}
                onSelectAgent={props.setBrokerAgentId}
              />
            ) : null}

            {state.brokerPage === "connectors" ? (
              <BrokerConnectorsSection
                canQuery={state.canQuery}
                isFetching={state.connectorsQuery.isFetching}
                error={state.connectorsQuery.error}
                connectors={state.connectors}
                connectorStaleMinutes={state.connectorStaleMinutes}
                setConnectorStaleMinutes={state.setConnectorStaleMinutes}
                connectorStaleMs={state.connectorStaleMs}
                onRefresh={() => void state.connectorsQuery.refetch()}
                onCopyJson={() => {
                  try {
                    void navigator.clipboard.writeText(JSON.stringify(state.connectors, null, 2));
                  } catch {
                    // ignore clipboard errors
                  }
                }}
                onDownloadJson={() => {
                  try {
                    const download = (payload: any) => {
                      const text = JSON.stringify(payload, null, 2);
                      const blob = new Blob([text], { type: "application/json" });
                      const url = URL.createObjectURL(blob);
                      const a = document.createElement("a");
                      a.href = url;
                      a.download = `broker-connectors-${Date.now()}.json`;
                      a.click();
                      URL.revokeObjectURL(url);
                    };
                    if (!state.canQuery) {
                      download(state.connectors);
                      return;
                    }
                    state
                      .exportConnectors()
                      .then((resp) => {
                        if (resp?.ok && Array.isArray(resp.connectors)) download(resp.connectors);
                        else download(state.connectors);
                      })
                      .catch(() => download(state.connectors));
                  } catch {
                    // ignore download errors
                  }
                }}
              />
            ) : null}

            {state.brokerPage === "members" ? (
              <BrokerMembersSection
                canQuery={state.canQuery}
                agentId={state.agentId}
                isFetching={state.membersQuery.isFetching}
                error={state.membersQuery.error}
                members={state.members}
                ownerSub={state.ownerSub}
                deletePending={state.deleteMutation.isPending}
                upsertPending={state.upsertMutation.isPending}
                newUserSub={state.newUserSub}
                newRole={state.newRole}
                actionError={state.actionError}
                setNewUserSub={state.setNewUserSub}
                setNewRole={state.setNewRole}
                onRefresh={() => void state.membersQuery.refetch()}
                onUpsert={() => void state.onUpsert()}
                onDelete={(userSub) => void state.onDelete(userSub)}
              />
            ) : null}

            {state.brokerPage === "deployments" ? (
              <BrokerDeploymentsSection
                canQuery={state.canQuery}
                agentId={state.agentId}
                isFetching={state.deploymentsQuery.isFetching}
                error={state.deploymentsQuery.error}
                deployments={state.deployments}
                defaultDeploymentId={state.defaultDeploymentId}
                selectedDeploymentSet={state.selectedDeploymentSet}
                selectedDeployments={state.selectedDeployments}
                toggleDeployment={state.toggleDeployment}
                selectConnectedDeployments={state.selectConnectedDeployments}
                selectAllDeployments={state.selectAllDeployments}
                onRefresh={() => void state.deploymentsQuery.refetch()}
                otaUrl={state.otaUrl}
                setOtaUrl={state.setOtaUrl}
                otaSha256={state.otaSha256}
                setOtaSha256={state.setOtaSha256}
                otaVersion={state.otaVersion}
                setOtaVersion={state.setOtaVersion}
                otaDrainMs={state.otaDrainMs}
                setOtaDrainMs={state.setOtaDrainMs}
                otaReason={state.otaReason}
                setOtaReason={state.setOtaReason}
                otaBusy={state.otaBusy}
                otaError={state.otaError}
                otaResults={state.otaResults}
                onRunOtaUpdate={() => void state.runOtaUpdate()}
                otaStatusBusy={state.otaStatusBusy}
                otaStatusError={state.otaStatusError}
                otaStatusResults={state.otaStatusResults}
                otaStatusCachedAt={state.otaStatusCachedAt}
                onRunOtaStatus={() => void state.runOtaStatus()}
                onClearOtaStatusCache={state.clearOtaStatusCache}
              />
            ) : null}

            {state.brokerPage === "memory" ? (
              <BrokerMemorySection
                agentId={state.agentId}
                selectedDeployments={state.selectedDeployments}
                retentionDryRun={state.retentionDryRun}
                setRetentionDryRun={state.setRetentionDryRun}
                retentionDailyMaxDays={state.retentionDailyMaxDays}
                setRetentionDailyMaxDays={state.setRetentionDailyMaxDays}
                retentionDailyMaxBytes={state.retentionDailyMaxBytes}
                setRetentionDailyMaxBytes={state.setRetentionDailyMaxBytes}
                retentionCheckpointMaxDays={state.retentionCheckpointMaxDays}
                setRetentionCheckpointMaxDays={state.setRetentionCheckpointMaxDays}
                retentionCheckpointMaxCount={state.retentionCheckpointMaxCount}
                setRetentionCheckpointMaxCount={state.setRetentionCheckpointMaxCount}
                retentionStructuredDeprecateDays={state.retentionStructuredDeprecateDays}
                setRetentionStructuredDeprecateDays={state.setRetentionStructuredDeprecateDays}
                retentionStructuredDeprecateMaxEntries={state.retentionStructuredDeprecateMaxEntries}
                setRetentionStructuredDeprecateMaxEntries={state.setRetentionStructuredDeprecateMaxEntries}
                retentionBusy={state.retentionBusy}
                retentionError={state.retentionError}
                retentionResults={state.retentionResults}
                onRunRetention={() => void state.runRetention()}
                onClearRetention={() => {
                  state.setRetentionError(null);
                  state.setRetentionResults(null);
                }}
                recapsLimit={state.recapsLimit}
                setRecapsLimit={state.setRecapsLimit}
                recapsIncludeSummary={state.recapsIncludeSummary}
                setRecapsIncludeSummary={state.setRecapsIncludeSummary}
                recapsDryRun={state.recapsDryRun}
                setRecapsDryRun={state.setRecapsDryRun}
                recapsWriteFile={state.recapsWriteFile}
                setRecapsWriteFile={state.setRecapsWriteFile}
                recapsKind={state.recapsKind}
                setRecapsKind={state.setRecapsKind}
                recapsKindFilter={state.recapsKindFilter}
                setRecapsKindFilter={state.setRecapsKindFilter}
                recapsModel={state.recapsModel}
                setRecapsModel={state.setRecapsModel}
                recapsSummaryMaxChars={state.recapsSummaryMaxChars}
                setRecapsSummaryMaxChars={state.setRecapsSummaryMaxChars}
                recapsDailyDays={state.recapsDailyDays}
                setRecapsDailyDays={state.setRecapsDailyDays}
                recapsMaxItems={state.recapsMaxItems}
                setRecapsMaxItems={state.setRecapsMaxItems}
                recapsStructuredMaxItems={state.recapsStructuredMaxItems}
                setRecapsStructuredMaxItems={state.setRecapsStructuredMaxItems}
                recapsDailyMaxItems={state.recapsDailyMaxItems}
                setRecapsDailyMaxItems={state.setRecapsDailyMaxItems}
                recapsHalfLifeDays={state.recapsHalfLifeDays}
                setRecapsHalfLifeDays={state.setRecapsHalfLifeDays}
                recapsImportanceWeight={state.recapsImportanceWeight}
                setRecapsImportanceWeight={state.setRecapsImportanceWeight}
                recapsIncludeStructured={state.recapsIncludeStructured}
                setRecapsIncludeStructured={state.setRecapsIncludeStructured}
                recapsIncludeDaily={state.recapsIncludeDaily}
                setRecapsIncludeDaily={state.setRecapsIncludeDaily}
                recapsListBusy={state.recapsListBusy}
                recapsGenerateBusy={state.recapsGenerateBusy}
                recapsError={state.recapsError}
                recapsResults={state.recapsResults}
                onRunRecapsList={() => void state.runRecapsList()}
                onRunRecapsGenerate={() => void state.runRecapsGenerate()}
                onClearRecaps={() => {
                  state.setRecapsError(null);
                  state.setRecapsResults(null);
                }}
                salienceBusy={state.salienceBusy}
                salienceError={state.salienceError}
                salienceResults={state.salienceResults}
                onRunSalience={() => void state.runSalience()}
                onClearSalience={() => {
                  state.setSalienceError(null);
                  state.setSalienceResults(null);
                }}
              />
            ) : null}

            {state.brokerPage === "audit" ? (
              <BrokerAuditSection
                canQuery={state.canQuery}
                agentId={state.agentId}
                isFetching={state.auditQuery.isFetching}
                error={state.auditQuery.error}
                auditLimit={state.auditLimit}
                setAuditLimit={state.setAuditLimit}
                auditRows={state.auditRows}
                onRefresh={() => void state.auditQuery.refetch()}
              />
            ) : null}

            {state.brokerPage === "events" ? (
              <BrokerEventsSection
                canQuery={state.canQuery}
                brokerEventsActive={state.brokerEventsState.brokerEventsActive}
                setBrokerEventsActive={state.brokerEventsState.setBrokerEventsActive}
                brokerEventsConnected={state.brokerEventsState.brokerEventsConnected}
                brokerEventsReplayBusy={state.brokerEventsState.brokerEventsReplayBusy}
                brokerEventsReplayNote={state.brokerEventsState.brokerEventsReplayNote}
                brokerEventsReplayError={state.brokerEventsState.brokerEventsReplayError}
                brokerEventsError={state.brokerEventsState.brokerEventsError}
                brokerEventsQuorumOnly={state.brokerEventsState.brokerEventsQuorumOnly}
                setBrokerEventsQuorumOnly={state.brokerEventsState.setBrokerEventsQuorumOnly}
                brokerEvents={state.brokerEventsState.brokerEvents}
                brokerEventRows={state.brokerEventsState.brokerEventRows}
                loadBrokerEventsReplay={state.brokerEventsState.loadBrokerEventsReplay}
                clearBrokerEvents={state.brokerEventsState.clearBrokerEvents}
              />
            ) : null}
          </div>
        </div>
      </div>
    </details>
  );
}
