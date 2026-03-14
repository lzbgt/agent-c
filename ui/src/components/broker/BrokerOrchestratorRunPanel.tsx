import React from "react";
import type { ApiAuth } from "../../api";
import BrokerOrchestratorRunMutationSection from "./BrokerOrchestratorRunMutationSection";
import BrokerOrchestratorRunOverviewSection from "./BrokerOrchestratorRunOverviewSection";
import BrokerOrchestratorRunRevisionsSection from "./BrokerOrchestratorRunRevisionsSection";
import useBrokerOrchestratorRunState from "./useBrokerOrchestratorRunState";

type OrchestratorRunPanelProps = {
  base: string;
  auth: ApiAuth;
  canQuery: boolean;
  teamId: string;
  teamMeta?: Record<string, any> | null;
  events?: Array<{ type?: string; ts_unix_ms?: number; event_id?: string; trace_id?: string; payload?: any }>;
};

export default function BrokerOrchestratorRunPanel(props: OrchestratorRunPanelProps) {
  const state = useBrokerOrchestratorRunState(props);

  return (
    <section className="rounded-md border border-white/10 bg-black/20 p-3" data-testid="orchestrator-run-panel">
      <div className="mb-2 text-xs font-semibold text-white/80">Orchestrator runs</div>
      <div className="grid gap-3">
        <BrokerOrchestratorRunOverviewSection canQuery={props.canQuery} state={state} />
        <BrokerOrchestratorRunRevisionsSection state={state} />
        <BrokerOrchestratorRunMutationSection canQuery={props.canQuery} state={state} />
      </div>
    </section>
  );
}
