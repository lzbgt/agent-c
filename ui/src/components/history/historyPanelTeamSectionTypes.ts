import type React from "react";
import type { HistoryPanelTeamState } from "./useHistoryPanelTeamState";

export type HistoryPanelTeamSectionProps = {
  teamId: string;
  teamRunId: string;
  teamRunCreatedMs: number;
  teamRunStatus: string;
  teamConversationItems: any[];
  teamConversationWarnings: string[];
  teamState: HistoryPanelTeamState;
  setShowSystemMessages: React.Dispatch<React.SetStateAction<boolean>>;
};
