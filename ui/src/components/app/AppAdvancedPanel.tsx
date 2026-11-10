import React from "react";
import type { AgentdTraceResp, ApiAuth, BrokerTraceResp } from "../../api";
import type { WorkflowDefaults } from "../../workflowTypes";
const ApprovalQueuePanel = React.lazy(() => import("../ApprovalQueuePanel"));
const BrokerPanel = React.lazy(() => import("../BrokerPanel"));
const MemoryPanel = React.lazy(() => import("../MemoryPanel"));
const RunDiffPanel = React.lazy(() => import("../RunDiffPanel"));
const TraceLookupPanel = React.lazy(() => import("../TraceLookupPanel"));
const VoicePanel = React.lazy(() => import("../VoicePanel"));
const WorkflowPanel = React.lazy(() => import("../WorkflowPanel"));

type AppAdvancedPanelProps = {
  advancedPage: string;
  setAdvancedPage: (next: string) => void;
  tracePanel: {
    traceId: string;
    onTraceIdChange: (next: string) => void;
    onLoad: (traceId: string) => void;
    onClear: () => void;
    loading: boolean;
    error: string | null;
    connectionMode: "direct" | "broker";
    baseUrl: string;
    yolo: boolean;
    agentdTrace: AgentdTraceResp | null;
    brokerTrace: BrokerTraceResp | null;
  };
  runDiffPanel: {
    baseUrl: string;
    auth: ApiAuth;
  };
  memoryPanel: {
    baseUrl: string;
    auth: ApiAuth;
  };
  voicePanel: {
    sessionId: string;
  };
  approvalsPanel: {
    baseUrl: string;
    auth: ApiAuth;
  };
  workflowPanel: {
    baseUrl: string;
    auth: ApiAuth;
    authKey?: string;
    clientId?: string;
    workflowDefaults?: WorkflowDefaults;
    workflowTargets?: string[];
    workflowBearerEnv?: string;
    onTraceIdClick: (traceId: string) => void;
  };
  brokerPanel: {
    enabled: boolean;
    brokerBase: string;
    daemonBase: string;
    brokerAgentId: string;
    setBrokerAgentId: (next: string) => void;
    auth: ApiAuth;
    authKey: string;
    clientId: string;
  };
};

export default function AppAdvancedPanel(props: AppAdvancedPanelProps) {
  if (!props.advancedPage) return null;

  return (
    <React.Suspense fallback={<div className="workspace-surface p-6 text-sm text-white/60" role="status">Loading workspace…</div>}>
    <section className="workspace-surface workspace-tool min-w-0 p-4" data-testid="workspace-tool">
      {props.advancedPage === "trace" ? (
        <TraceLookupPanel
          traceId={props.tracePanel.traceId}
          onTraceIdChange={props.tracePanel.onTraceIdChange}
          onLoad={props.tracePanel.onLoad}
          onClear={props.tracePanel.onClear}
          loading={props.tracePanel.loading}
          error={props.tracePanel.error}
          connectionMode={props.tracePanel.connectionMode}
          baseUrl={props.tracePanel.baseUrl}
          yolo={props.tracePanel.yolo}
          agentdTrace={props.tracePanel.agentdTrace}
          brokerTrace={props.tracePanel.brokerTrace}
        />
      ) : props.advancedPage === "run-diff" ? (
        <RunDiffPanel
          baseUrl={props.runDiffPanel.baseUrl}
          auth={props.runDiffPanel.auth}
        />
      ) : props.advancedPage === "memory" ? (
        <MemoryPanel
          baseUrl={props.memoryPanel.baseUrl}
          auth={props.memoryPanel.auth}
        />
      ) : props.advancedPage === "voice" ? (
        <VoicePanel
          sessionId={props.voicePanel.sessionId}
        />
      ) : props.advancedPage === "approvals" ? (
        <ApprovalQueuePanel
          baseUrl={props.approvalsPanel.baseUrl}
          auth={props.approvalsPanel.auth}
        />
      ) : props.advancedPage === "workflows" ? (
        <WorkflowPanel
          open={true}
          baseUrl={props.workflowPanel.baseUrl}
          auth={props.workflowPanel.auth}
          authKey={props.workflowPanel.authKey}
          clientId={props.workflowPanel.clientId}
          workflowDefaults={props.workflowPanel.workflowDefaults}
          workflowTargets={props.workflowPanel.workflowTargets}
          workflowBearerEnv={props.workflowPanel.workflowBearerEnv}
          onTraceIdClick={props.workflowPanel.onTraceIdClick}
        />
      ) : props.advancedPage === "broker" && props.brokerPanel.enabled ? (
        <BrokerPanel
          open={true}
          onToggle={(open) => {
            if (!open) props.setAdvancedPage("");
          }}
          brokerBase={props.brokerPanel.brokerBase}
          daemonBase={props.brokerPanel.daemonBase}
          brokerAgentId={props.brokerPanel.brokerAgentId}
          setBrokerAgentId={props.brokerPanel.setBrokerAgentId}
          auth={props.brokerPanel.auth}
          authKey={props.brokerPanel.authKey}
          clientId={props.brokerPanel.clientId}
        />
      ) : null}
    </section>
    </React.Suspense>
  );
}
