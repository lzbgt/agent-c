import React from "react";
import type { ClientSettings } from "../../hooks/useUiSettings";
import FieldLabel from "../FieldLabel";
import { SectionHeader, ToggleRow } from "./SettingsControls";

type SettingsClientPreferencesSectionProps = {
  client: ClientSettings;
  connectorStaleMinutes: string;
  setConnectorStaleMinutes: (next: string) => void;
  daemonConfig: {
    isFetching: boolean;
    refresh: () => void;
  };
};

export default function SettingsClientPreferencesSection(props: SettingsClientPreferencesSectionProps) {
  const { client, connectorStaleMinutes, setConnectorStaleMinutes, daemonConfig } = props;

  return (
    <div className="mt-4 rounded-md border border-white/10 bg-black/20 p-3">
      <SectionHeader
        title="Client"
        action={
          <button
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
            onClick={() => daemonConfig.refresh()}
            type="button"
            disabled={daemonConfig.isFetching}
          >
            Refresh config
          </button>
        }
      />
      <div className="mt-2 grid gap-2 text-[11px] text-white/70">
        <ToggleRow label="Allow audio autoplay" checked={client.allowAutoplay} onChange={client.setAllowAutoplay} />
        <ToggleRow label="Allow client RPCs" checked={client.allowClientRpcs} onChange={client.setAllowClientRpcs} />
        <ToggleRow
          label="Allow client RPC side effects"
          checked={client.allowClientEffects}
          onChange={client.setAllowClientEffects}
          disabled={!client.allowClientRpcs}
        />
        <ToggleRow
          label="Allow unsafe page eval"
          checked={client.allowUnsafePageEval}
          onChange={client.setAllowUnsafePageEval}
        />
        <div className="grid gap-1">
          <FieldLabel>Connector stale after (minutes)</FieldLabel>
          <input
            className="w-32 rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
            value={connectorStaleMinutes}
            onChange={(e) => setConnectorStaleMinutes(e.target.value)}
            inputMode="numeric"
          />
          <div className="text-[10px] text-white/50">Used by the broker connectors list (local setting).</div>
        </div>
      </div>
    </div>
  );
}
