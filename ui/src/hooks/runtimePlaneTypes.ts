import type React from "react";
import type { AgentEvent, ApiAuth } from "../api";
import type {
  DbClientEventRow,
  DbUiActionRow,
  SessionArtifactRow,
  SessionSceneSnapshot,
} from "../history/historyPanelData";
import type { RunWatchByScope } from "../runWatchPrefs";

export type JobStoreWriter = (mutate: (prev: RunWatchByScope) => RunWatchByScope) => void;

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
  setLiveEvents: React.Dispatch<React.SetStateAction<AgentEvent[]>>;
  yolo: boolean;
  artifactCatalogSupported: boolean;
  dbClientEventRows: DbClientEventRow[];
  dbUiActionRows: DbUiActionRow[];
  sessionArtifactRows: SessionArtifactRow[];
  sessionSceneSnapshot: SessionSceneSnapshot | null;
};
