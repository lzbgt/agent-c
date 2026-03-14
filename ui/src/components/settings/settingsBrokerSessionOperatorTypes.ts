import type { ClientSettings, ConnectionSettings } from "../../hooks/uiSettingsTypes";

export type SessionSettings = {
  id: string;
  leaseSeconds: string;
  info?: {
    attachment?: {
      client_id?: string | null;
      lease_active?: boolean;
    };
  };
};

export type SettingsBrokerSessionOperatorsSectionProps = {
  connection: ConnectionSettings;
  client: ClientSettings;
  session: SessionSettings;
};
