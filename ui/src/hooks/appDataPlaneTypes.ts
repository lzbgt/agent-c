import type React from "react";
import type {
  AgentEvent,
  ApiAuth,
  RunResponse,
  SessionAttachment,
  SessionInfo,
} from "../api";

export type AppDataPlaneArgs = {
  activeJobId: string | null;
  allowClientEffects: boolean;
  allowClientRpcs: boolean;
  apiKey: string;
  authKey: string;
  baseUrl: string;
  brokerAuthToken: string;
  brokerCookieAuth: boolean;
  clientId: string;
  connectionMode: string;
  daemonAuth: ApiAuth;
  daemonAuthToken: string;
  effectiveBase: string;
  jobStatus: string | null;
  jobUpdatedMs: number | null;
  lastRunPrompt: string;
  lastRunPromptRef: React.MutableRefObject<string>;
  liveEvents: AgentEvent[];
  model: string;
  proxyUrl: string;
  selectedSessionId: string;
  setActiveJobId: React.Dispatch<React.SetStateAction<string | null>>;
  setJobError: React.Dispatch<React.SetStateAction<string | null>>;
  setJobStatus: React.Dispatch<React.SetStateAction<string | null>>;
  setJobUpdatedMs: React.Dispatch<React.SetStateAction<number | null>>;
  setLastCompletedPrompt: React.Dispatch<React.SetStateAction<string>>;
  setLastRunPrompt: React.Dispatch<React.SetStateAction<string>>;
  setLiveEvents: React.Dispatch<React.SetStateAction<AgentEvent[]>>;
  setPrompt: React.Dispatch<React.SetStateAction<string>>;
  setResult: React.Dispatch<React.SetStateAction<RunResponse | undefined>>;
  setSessionId: (sid: string) => void;
  sessionLeaseSeconds: string;
  summaryMaxChars: string;
  summaryModel: string;
  timeoutMs: string;
  cursorRef: React.MutableRefObject<number>;
};

export type SessionLeaseConflict = {
  requestedClientId: string | null;
  currentAttachment?: SessionAttachment;
  code: string;
  message: string;
  retryable: boolean;
};

export type SessionInfoData = SessionInfo | undefined;
