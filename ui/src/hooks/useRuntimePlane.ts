import { useRunWatchState } from "./useRunWatchState";
import { useRuntimeClientEffects } from "./useRuntimeClientEffects";
import type { RuntimePlaneArgs } from "./runtimePlaneTypes";

export type { JobStoreWriter, RuntimePlaneArgs } from "./runtimePlaneTypes";

export default function useRuntimePlane(args: RuntimePlaneArgs) {
  const runWatchState = useRunWatchState({
    activeJobId: args.activeJobId,
    authKey: args.authKey,
    cursorRef: args.cursorRef,
    daemonAuth: args.daemonAuth,
    effectiveBase: args.effectiveBase,
    jobStoreKey: args.jobStoreKey,
    runWatchCanUse: args.runWatchCanUse,
    runWatchPrefsBase: args.runWatchPrefsBase,
    runWatchPrefsClientId: args.runWatchPrefsClientId,
    sessionId: args.sessionId,
    setActiveJobId: args.setActiveJobId,
    setJobError: args.setJobError,
    setJobStatus: args.setJobStatus,
    setJobUpdatedMs: args.setJobUpdatedMs,
    setLiveEvents: args.setLiveEvents,
  });

  const runtimeClientEffects = useRuntimeClientEffects({
    allowClientEffects: args.allowClientEffects,
    allowClientRpcs: args.allowClientRpcs,
    artifactCatalogSupported: args.artifactCatalogSupported,
    client: args.client,
    daemonAuth: args.daemonAuth,
    dbClientEventRows: args.dbClientEventRows,
    dbUiActionRows: args.dbUiActionRows,
    effectiveBase: args.effectiveBase,
    sessionArtifactRows: args.sessionArtifactRows,
    sessionId: args.sessionId,
    sessionSceneSnapshot: args.sessionSceneSnapshot,
    sessionScopeKey: args.sessionScopeKey,
    yolo: args.yolo,
  });

  return {
    applySceneOps: runtimeClientEffects.applySceneOps,
    runWatchMode: runWatchState.runWatchMode,
    sceneEntities: runtimeClientEffects.sceneEntities,
    writeJobsBySession: runWatchState.writeJobsBySession,
  };
}
