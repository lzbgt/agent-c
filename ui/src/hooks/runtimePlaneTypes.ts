import type React from "react";
import type { ApiAuth } from "../api";

export type JobStoreWriter = (mutate: (prev: Record<string, any>) => Record<string, any>) => void;

export type RuntimePlaneArgs = {
  activeJobId: string | null;
  allowClientEffects: boolean;
  allowClientRpcs: boolean;
  authKey: string;
  client: { id: string; kind: string; instance_id: string };
  cursorRef: React.MutableRefObject<number>;
  daemonAuth: ApiAuth;
  effectiveBase: string;
  jobStoreKey: string;
  runWatchCanUse: boolean;
  runWatchPrefsBase: string;
  runWatchPrefsClientId: string;
  sessionId: string;
  sessionScopeKey: string;
  setActiveJobId: React.Dispatch<React.SetStateAction<string | null>>;
  setJobError: React.Dispatch<React.SetStateAction<string | null>>;
  setJobStatus: React.Dispatch<React.SetStateAction<string | null>>;
  setJobUpdatedMs: React.Dispatch<React.SetStateAction<number | null>>;
  setLiveEvents: React.Dispatch<React.SetStateAction<any[]>>;
  yolo: boolean;
  artifactCatalogSupported: boolean;
  dbClientEventsData: any;
  dbUiActionsData: any;
  sessionArtifactsData: any;
  sessionSceneData: any;
};
