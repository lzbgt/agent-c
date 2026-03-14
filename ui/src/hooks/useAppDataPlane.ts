import { useAppSessionMutations } from "./useAppSessionMutations";
import { useAppSessionQueries } from "./useAppSessionQueries";
import type { AppDataPlaneArgs } from "./appDataPlaneTypes";

export type { AppDataPlaneArgs, SessionLeaseConflict } from "./appDataPlaneTypes";

export default function useAppDataPlane(args: AppDataPlaneArgs) {
  const queries = useAppSessionQueries(args);
  const mutations = useAppSessionMutations({
    activeJobId: args.activeJobId,
    apiKey: args.apiKey,
    baseUrl: args.baseUrl,
    clientId: args.clientId,
    daemonAuth: args.daemonAuth,
    effectiveBase: args.effectiveBase,
    lastRunPromptRef: args.lastRunPromptRef,
    model: args.model,
    proxyUrl: args.proxyUrl,
    selectedSessionId: args.selectedSessionId,
    sessionLeaseSeconds: args.sessionLeaseSeconds,
    summaryMaxChars: args.summaryMaxChars,
    summaryModel: args.summaryModel,
    timeoutMs: args.timeoutMs,
    cursorRef: args.cursorRef,
    setActiveJobId: args.setActiveJobId,
    setJobError: args.setJobError,
    setJobStatus: args.setJobStatus,
    setJobUpdatedMs: args.setJobUpdatedMs,
    setLastCompletedPrompt: args.setLastCompletedPrompt,
    setLastRunPrompt: args.setLastRunPrompt,
    setLiveEvents: args.setLiveEvents,
    setPrompt: args.setPrompt,
    setResult: args.setResult,
    setSessionId: args.setSessionId,
    sessionInfo: queries.sessionInfo,
    audit: queries.audit,
    sessions: queries.sessions,
    sessionClientEvents: queries.sessionClientEvents,
    sessionArtifacts: queries.sessionArtifacts,
    sessionScene: queries.sessionScene,
    dbUiActions: queries.dbUiActions,
    dbClientEvents: queries.dbClientEvents,
    daemonConfig: queries.daemonConfig,
  });

  return {
    ...queries,
    ...mutations,
  };
}
