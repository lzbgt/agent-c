import type React from "react";
import type { TeamConversationItem } from "../../hooks/teamChatOrchestrationTypes";
import type { HistoryPanelTeamState } from "./useHistoryPanelTeamState";

export type HistoryPanelTeamSectionProps = {
  teamId: string;
  teamRunId: string;
  teamRunCreatedMs: number;
  teamRunStatus: string;
  teamConversationItems: TeamConversationItem[];
  teamConversationWarnings: string[];
  teamState: HistoryPanelTeamState;
  setShowSystemMessages: React.Dispatch<React.SetStateAction<boolean>>;
};
