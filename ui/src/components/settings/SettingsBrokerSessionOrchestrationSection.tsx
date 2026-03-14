import React from "react";

import { jsonText } from "./settingsBrokerSessionOperatorUtils";

type Props = {
  orchestrationStatus: { isFetching: boolean; data: unknown };
  orchestrationWorkers: { isFetching: boolean; data: unknown };
  orchestrationDependencies: { isFetching: boolean; data: unknown };
};

export default function SettingsBrokerSessionOrchestrationSection(props: Props) {
  return (
    <div className="mt-4 rounded-md border border-white/10 bg-black/20 p-3">
      <div className="flex items-center justify-between gap-2">
        <div className="text-xs font-semibold text-white/80">Orchestration</div>
        <div className="text-[11px] text-white/50">
          status {props.orchestrationStatus.isFetching ? "loading" : "ready"} · workers {props.orchestrationWorkers.isFetching ? "loading" : "ready"} · dependencies{" "}
          {props.orchestrationDependencies.isFetching ? "loading" : "ready"}
        </div>
      </div>
      <div className="mt-2 grid grid-cols-1 gap-3">
        <div>
          <div className="text-[11px] text-white/60">Status</div>
          <pre className="mt-1 max-h-32 overflow-auto rounded-md border border-white/10 bg-black/30 p-2 text-[11px] text-white/80">{jsonText(props.orchestrationStatus.data || {})}</pre>
        </div>
        <div>
          <div className="text-[11px] text-white/60">Workers</div>
          <pre className="mt-1 max-h-32 overflow-auto rounded-md border border-white/10 bg-black/30 p-2 text-[11px] text-white/80">{jsonText(props.orchestrationWorkers.data || {})}</pre>
        </div>
        <div>
          <div className="text-[11px] text-white/60">Dependencies</div>
          <pre className="mt-1 max-h-32 overflow-auto rounded-md border border-white/10 bg-black/30 p-2 text-[11px] text-white/80">{jsonText(props.orchestrationDependencies.data || {})}</pre>
        </div>
      </div>
    </div>
  );
}
