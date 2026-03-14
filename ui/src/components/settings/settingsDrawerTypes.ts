import type { Caps, DaemonConfigResp, SessionInfo } from "../../api";
import type { ClientSettings, ConnectionSettings, RunSettings } from "../../hooks/useUiSettings";

export type SettingsDrawerProps = {
  open: boolean;
  onClose: () => void;
  connection: ConnectionSettings;
  run: RunSettings;
  client: ClientSettings;
  session: {
    id: string;
    setId: (next: string) => void;
    leaseSeconds: string;
    setLeaseSeconds: (next: string) => void;
    info?: SessionInfo;
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
    sessions: string[];
    refresh: () => void;
    newSession: () => void;
    newSessionPending: boolean;
    deleteSession: (sid: string) => void;
    deletePending: boolean;
    deleteError: string | null;
    clearAll: () => void;
    clearAllPending: boolean;
    clearAllError: string | null;
  };
  daemonConfig: {
    data?: DaemonConfigResp;
    isFetching: boolean;
    refresh: () => void;
  };
  updateDaemonDefaults: {
    pending: boolean;
    error: string | null;
    success: boolean;
    saveDefaults: () => void;
    saveApiKey: () => void;
    clearApiKey: () => void;
  };
  caps: {
    data?: Caps;
    source: "live" | "cache" | "none";
    updatedMs?: number;
    isFetching: boolean;
    error: string | null;
    refresh: () => void;
  };
};
