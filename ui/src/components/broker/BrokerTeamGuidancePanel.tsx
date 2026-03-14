import React from "react";

import useBrokerTeamGuidanceState from "./useBrokerTeamGuidanceState";
import BrokerTeamGuidanceComposerSection from "./BrokerTeamGuidanceComposerSection";
import BrokerTeamGuidanceListSection from "./BrokerTeamGuidanceListSection";
import type { BrokerTeamGuidancePanelProps } from "./brokerTeamGuidanceTypes";

export type { BrokerTeamGuidancePanelProps } from "./brokerTeamGuidanceTypes";

export default function BrokerTeamGuidancePanel(props: BrokerTeamGuidancePanelProps) {
  const state = useBrokerTeamGuidanceState(props);
  return (
    <div className="space-y-3 rounded-lg border border-white/10 bg-white/5 p-3">
      <BrokerTeamGuidanceComposerSection {...state} />
      <BrokerTeamGuidanceListSection {...state} />
    </div>
  );
}
