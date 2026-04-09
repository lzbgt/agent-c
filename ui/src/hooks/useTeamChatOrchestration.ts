import React from "react";
import { useTeamChatActionState } from "./useTeamChatActionState";
import { useTeamChatConversationState } from "./useTeamChatConversationState";
import type { TeamChatOrchestrationArgs } from "./teamChatOrchestrationTypes";

export type {
  TeamActionKind,
  TeamChatOrchestrationArgs,
  TeamConversationItem,
  TeamQueuedAction,
} from "./teamChatOrchestrationTypes";

export default function useTeamChatOrchestration(args: TeamChatOrchestrationArgs) {
  const {
    brokerAgentId,
    brokerBase,
    brokerChatAvailable,
    connectionMode,
    daemonAuth,
    selectedTeamId,
    setAdvancedPage,
    setComposerTaskNonce,
    setJobNotice,
    setPrompt,
  } = args;

  const conversationState = useTeamChatConversationState({
    authKey: args.authKey,
    brokerAgentId,
    brokerBase,
    brokerChatAvailable,
    daemonAuth,
    selectedTeamId,
  });

  const openTeamPanel = React.useCallback(
    (tab: string) => {
      if (connectionMode !== "broker") return;
      try {
        if (conversationState.selectedTeamIdTrimmed) {
          window.localStorage.setItem("agentui.brokerTeamId", conversationState.selectedTeamIdTrimmed);
        }
        if (tab) {
          window.localStorage.setItem("agentui.teamTab", tab);
        }
      } catch {
        // ignore localStorage failures
      }
      setAdvancedPage("broker");
    },
    [connectionMode, conversationState.selectedTeamIdTrimmed, setAdvancedPage],
  );

  const actionState = useTeamChatActionState({
    addTeamActivity: conversationState.addTeamActivity,
    brokerAgentId,
    brokerBaseTrimmed: conversationState.brokerBaseTrimmed,
    brokerChatAvailable,
    daemonAuth,
    latestTeamRunId: conversationState.latestTeamRunId,
    refetchTeamGuidance: conversationState.teamGuidance.refetch,
    refetchTeamRuns: conversationState.teamRunList.refetch,
    selectedTeamIdTrimmed: conversationState.selectedTeamIdTrimmed,
    setComposerTaskNonce,
    setJobNotice,
    setPrompt,
    setTeamQueue: conversationState.setTeamQueue,
    teamQueue: conversationState.teamQueue,
  });

  return {
    clearTeamQueue: actionState.clearTeamQueue,
    handleTeamRunRequest: actionState.handleTeamRunRequest,
    latestTeamRunCreatedMs: conversationState.latestTeamRunCreatedMs,
    latestTeamRunId: conversationState.latestTeamRunId,
    openTeamPanel,
    teamConversationCacheUpdatedMs: conversationState.teamConversationCacheUpdatedMs,
    teamConversationItems: conversationState.teamConversationItems,
    teamConversationUsingCache: conversationState.teamConversationUsingCache,
    teamConversationWarnings: conversationState.teamConversationWarnings,
    teamQueue: conversationState.teamQueue,
    teamQueueCount: conversationState.teamQueueCount,
    teamQueueNeedsRun: conversationState.teamQueueNeedsRun,
    teamRecentActivity: conversationState.teamRecentActivity,
    teamRunCreate: actionState.teamRunCreate,
    teamStatus: conversationState.teamStatus,
  };
}
