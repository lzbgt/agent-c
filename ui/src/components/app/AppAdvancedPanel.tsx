import React from "react";
import type { ApiAuth } from "../../api";
import ApprovalQueuePanel from "../ApprovalQueuePanel";
import BrokerPanel from "../BrokerPanel";
import MemoryPanel from "../MemoryPanel";
import RunDiffPanel from "../RunDiffPanel";
import TraceLookupPanel from "../TraceLookupPanel";
import WorkflowPanel from "../WorkflowPanel";

type AppAdvancedPanelProps = {
  advancedPage: string;
  focusAdvancedPanel: boolean;
  setFocusAdvancedPanel: React.Dispatch<React.SetStateAction<boolean>>;
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
    agentdTrace: any | null;
    brokerTrace: any | null;
  };
  runDiffPanel: {
    baseUrl: string;
    auth: ApiAuth;
  };
  memoryPanel: {
    baseUrl: string;
    auth: ApiAuth;
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
    workflowDefaults?: Record<string, any>;
    workflowTargets?: string[];
    workflowBearerEnv?: string;
    onTraceIdClick: (traceId: string) => void;
  };
  brokerPanel: {
    enabled: boolean;
    brokerBase: string;
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
    <aside className="min-w-0 overflow-x-auto rounded-lg border border-white/10 bg-black/20 p-3">
      <div className="mb-2 flex items-center justify-between gap-2">
        <div className="text-[11px] font-semibold text-white/60">Panel view</div>
        <button
          className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-[10px] text-white/70 hover:bg-black/40"
          type="button"
          onClick={() => props.setFocusAdvancedPanel((prev) => !prev)}
          title={
            props.focusAdvancedPanel
              ? "Show scene and history again"
              : "Hide scene + history to focus on this panel"
          }
        >
          {props.focusAdvancedPanel ? "Show main view" : "Focus panel"}
        </button>
      </div>
      {props.advancedPage === "trace" ? (
        <TraceLookupPanel
          open={true}
          onToggle={(open) => {
            if (!open) props.setAdvancedPage("");
          }}
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
          open={true}
          onToggle={(open) => {
            if (!open) props.setAdvancedPage("");
          }}
          baseUrl={props.runDiffPanel.baseUrl}
          auth={props.runDiffPanel.auth}
        />
      ) : props.advancedPage === "memory" ? (
        <MemoryPanel
          open={true}
          onToggle={(open) => {
            if (!open) props.setAdvancedPage("");
          }}
          baseUrl={props.memoryPanel.baseUrl}
          auth={props.memoryPanel.auth}
        />
      ) : props.advancedPage === "approvals" ? (
        <ApprovalQueuePanel
          open={true}
          onToggle={(open) => {
            if (!open) props.setAdvancedPage("");
          }}
          baseUrl={props.approvalsPanel.baseUrl}
          auth={props.approvalsPanel.auth}
        />
      ) : props.advancedPage === "workflows" ? (
        <WorkflowPanel
          open={true}
          onToggle={(open) => {
            if (!open) props.setAdvancedPage("");
          }}
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
          brokerAgentId={props.brokerPanel.brokerAgentId}
          setBrokerAgentId={props.brokerPanel.setBrokerAgentId}
          auth={props.brokerPanel.auth}
          authKey={props.brokerPanel.authKey}
          clientId={props.brokerPanel.clientId}
        />
      ) : null}
    </aside>
  );
}
