import React from "react";

import SectionCard from "./BrokerTeamSectionCard";
import BrokerTeamMembersFormSection from "./BrokerTeamMembersFormSection";
import BrokerTeamMembersListSection from "./BrokerTeamMembersListSection";
import type { BrokerTeamMembersPanelProps } from "./brokerTeamMembersTypes";

export type { BrokerTeamMembersPanelProps } from "./brokerTeamMembersTypes";

export default function BrokerTeamMembersPanel(props: BrokerTeamMembersPanelProps) {
  return (
    <SectionCard title="Team members" description="Add members, update status, or edit overrides.">
      <BrokerTeamMembersFormSection {...props} />
      <BrokerTeamMembersListSection {...props} />
    </SectionCard>
  );
}
