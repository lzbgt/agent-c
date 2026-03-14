import React from "react";

import FieldLabel from "../FieldLabel";
import SettingsBrokerSessionCapabilitiesSection from "./SettingsBrokerSessionCapabilitiesSection";
import SettingsBrokerSessionOrchestrationSection from "./SettingsBrokerSessionOrchestrationSection";
import SettingsBrokerSessionServicesSection from "./SettingsBrokerSessionServicesSection";
import SettingsBrokerSessionShellsSection from "./SettingsBrokerSessionShellsSection";
import type { SettingsBrokerSessionOperatorsSectionProps } from "./settingsBrokerSessionOperatorTypes";
import useBrokerSessionOperatorsState from "./useBrokerSessionOperatorsState";

export default function SettingsBrokerSessionOperatorsSection(props: SettingsBrokerSessionOperatorsSectionProps) {
  const state = useBrokerSessionOperatorsState(props);

  if (props.connection.mode !== "broker") return null;

  return (
    <div className="mt-4 rounded-md border border-white/10 bg-black/10 p-3" data-testid="broker-session-operators-section">
      <div className="flex items-center justify-between gap-2">
        <div>
          <FieldLabel>Broker session operators</FieldLabel>
          <div className="mt-1 text-[11px] text-white/60">
            Shell-first host examination for the current broker session. Artifact browsing is still intentionally transcript/event/shell based.
          </div>
        </div>
        <button
          className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
          type="button"
          onClick={state.refreshOperatorState}
          disabled={!state.enabled}
        >
          Refresh all
        </button>
      </div>

      {!state.sessionId ? <div className="mt-3 text-[11px] text-white/60">Select or create a session first.</div> : null}
      {state.sessionId ? (
        <div className="mt-2 text-[11px] text-white/60">
          lease role: <span className="font-mono text-white/80">{state.leaseRole}</span>
          {state.leaseRole !== "owner" ? " · lease-owned shell/service mutations may return attachment_conflict." : ""}
        </div>
      ) : null}

      {state.enabled ? (
        <>
          <SettingsBrokerSessionOrchestrationSection
            orchestrationStatus={state.orchestrationStatus}
            orchestrationWorkers={state.orchestrationWorkers}
            orchestrationDependencies={state.orchestrationDependencies}
          />

          <SettingsBrokerSessionShellsSection
            shellCommand={state.shellCommand}
            setShellCommand={state.setShellCommand}
            shellIntent={state.shellIntent}
            setShellIntent={state.setShellIntent}
            shellLabel={state.shellLabel}
            setShellLabel={state.setShellLabel}
            selectedShellRef={state.selectedShellRef}
            setSelectedShellRef={state.setSelectedShellRef}
            shellInput={state.shellInput}
            setShellInput={state.setShellInput}
            shellNotice={state.shellNotice}
            shellRows={state.shellRows}
            shellDetail={state.shellDetail}
            startShell={state.startShell}
            pollShell={state.pollShell}
            sendShell={state.sendShell}
            terminateShell={state.terminateShell}
          />

          <SettingsBrokerSessionServicesSection
            selectedServiceRef={state.selectedServiceRef}
            setSelectedServiceRef={state.setSelectedServiceRef}
            serviceWaitMs={state.serviceWaitMs}
            setServiceWaitMs={state.setServiceWaitMs}
            serviceRecipe={state.serviceRecipe}
            setServiceRecipe={state.setServiceRecipe}
            serviceArgsJson={state.serviceArgsJson}
            setServiceArgsJson={state.setServiceArgsJson}
            serviceNotice={state.serviceNotice}
            serviceRows={state.serviceRows}
            serviceDetail={state.serviceDetail}
            attachService={state.attachService}
            waitService={state.waitService}
            runService={state.runService}
          />

          <SettingsBrokerSessionCapabilitiesSection
            selectedCapabilityRef={state.selectedCapabilityRef}
            setSelectedCapabilityRef={state.setSelectedCapabilityRef}
            capabilityRows={state.capabilityRows}
            capabilityDetail={state.capabilityDetail}
          />

          {state.operatorReadError ? <div className="mt-3 text-[11px] text-rose-200">operator read error: {state.operatorReadError}</div> : null}
        </>
      ) : null}
    </div>
  );
}
