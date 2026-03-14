import type { ClientSettings, ConnectionSettings, RunSettings } from "../../hooks/useUiSettings";

export type SessionSettings = {
  id: string;
  setId: (next: string) => void;
  leaseSeconds: string;
  setLeaseSeconds: (next: string) => void;
  info?: {
    session_id?: string;
    thread_id?: string | null;
    working?: boolean;
    attachment?: {
      client_id?: string | null;
      lease_seconds?: number | null;
      lease_expires_at_ms?: number | null;
      lease_active?: boolean;
    };
  };
  leaseConflict: {
    requestedClientId: string | null;
    currentAttachment?: {
      client_id?: string | null;
      lease_seconds?: number | null;
      lease_expires_at_ms?: number | null;
      lease_active?: boolean;
    };
    code: string;
    message: string;
    retryable: boolean;
  } | null;
  clearLeaseConflict: () => void;
  attach: () => void;
  attachPending: boolean;
  attachError: string | null;
  renewAttachment: () => void;
  renewPending: boolean;
  renewError: string | null;
  releaseAttachment: () => void;
  releasePending: boolean;
  releaseError: string | null;
  streamStatus: "disabled" | "idle" | "connecting" | "live" | "reconnecting" | "error";
  streamLastEventId: string;
  streamLastEventAtMs: number | null;
  streamUpdatedMs: number | null;
  streamBufferedCount: number;
  streamError: string | null;
};

export type SettingsConnectionSectionProps = {
  connection: ConnectionSettings;
  run: RunSettings;
  client: ClientSettings;
  session: SessionSettings;
  serverPrefsCanSync: boolean;
  serverPrefsTarget: string;
  serverPrefsStatusLabel: string;
  serverPrefsAutoNote: string | null;
  brokerAuthReady: boolean;
  brokerAgentsBusy: boolean;
  brokerAgentsError: string | null;
  brokerAgents: any[] | null;
  brokerDeploymentsBusy: boolean;
  brokerDeploymentsError: string | null;
  brokerDeployments: any[] | null;
  brokerDeploymentsDefaultId: string | null;
  listBrokerAgents: () => Promise<void>;
  listBrokerDeployments: () => Promise<void>;
  automationProfiles: string[];
  automationDefault: string;
  automationOverrideAllowed: boolean;
};
