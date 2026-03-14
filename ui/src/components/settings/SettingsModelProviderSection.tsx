import React from "react";
import type { DaemonConfigResp } from "../../api";
import type { ClientSettings, ConnectionSettings, RunSettings } from "../../hooks/uiSettingsTypes";
import FieldLabel from "../FieldLabel";
import { SectionHeader, ToggleRow } from "./SettingsControls";

type SettingsModelProviderSectionProps = {
  connection: ConnectionSettings;
  run: RunSettings;
  client: ClientSettings;
  daemonDefaults?: DaemonConfigResp["daemon"];
  jobsEnabled: boolean;
  baseUrlLabel: string;
};

export default function SettingsModelProviderSection(props: SettingsModelProviderSectionProps) {
  const { connection, run, client, daemonDefaults, jobsEnabled, baseUrlLabel } = props;

  return (
    <div className="mt-4 rounded-md border border-white/10 bg-black/20 p-3">
      <SectionHeader
        title="Model / Provider"
        action={
          <button
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
            type="button"
            onClick={() => {
              run.setBaseUrl(daemonDefaults?.base_url || run.baseUrl);
              run.setModel(daemonDefaults?.model || run.model);
              run.setSummaryModel(daemonDefaults?.summary_model || "");
              run.setSummaryMaxChars(
                typeof daemonDefaults?.summary_max_chars === "number"
                  ? String(daemonDefaults.summary_max_chars)
                  : run.summaryMaxChars,
              );
              run.setTimeoutMs(typeof daemonDefaults?.timeout_ms === "number" ? String(daemonDefaults.timeout_ms) : run.timeoutMs);
            }}
            title="Copy daemon defaults into local fields"
          >
            Use daemon defaults
          </button>
        }
      />
      <div className="mt-2 flex flex-wrap items-center gap-2 text-[11px] text-white/60">
        <label className="flex items-center gap-2">
          <input
            type="checkbox"
            checked={run.profileOverridesEnabled}
            onChange={(e) => run.setProfileOverridesEnabled(e.target.checked)}
          />
          <span>Profile-specific run settings</span>
        </label>
        <span className="text-white/40">Applies to {connection.profileName}</span>
        <button
          className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
          type="button"
          onClick={() => run.copyProfileOverridesFromGlobal()}
          disabled={!run.profileOverridesEnabled}
          title="Copy global run settings into this profile"
        >
          Sync from global
        </button>
        <button
          className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
          type="button"
          onClick={() => run.clearProfileOverrides()}
          disabled={!run.profileOverridesEnabled}
          title="Disable and clear profile overrides"
        >
          Revert to global
        </button>
      </div>
      <div className="mt-3 grid gap-3 text-[11px] text-white/70">
        <div>
          <FieldLabel>Base URL</FieldLabel>
          <input
            className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
            value={run.baseUrl}
            onChange={(e) => run.setBaseUrl(e.target.value)}
          />
          <div className="mt-1 text-white/50">Active: {baseUrlLabel || "(empty)"}</div>
        </div>
        <div>
          <FieldLabel>Model</FieldLabel>
          <input
            className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
            value={run.model}
            onChange={(e) => run.setModel(e.target.value)}
          />
        </div>
        <div>
          <FieldLabel>Summary model (optional)</FieldLabel>
          <input
            className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
            value={run.summaryModel}
            onChange={(e) => run.setSummaryModel(e.target.value)}
            placeholder="Leave blank to disable summaries"
          />
        </div>
        <div>
          <FieldLabel>Summary max chars</FieldLabel>
          <input
            className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
            value={run.summaryMaxChars}
            onChange={(e) => run.setSummaryMaxChars(e.target.value)}
            inputMode="numeric"
          />
        </div>
        <div>
          <FieldLabel>API key (local)</FieldLabel>
          <input
            className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
            value={run.apiKey}
            onChange={(e) => run.setApiKey(e.target.value)}
            placeholder="Stored in browser storage"
          />
        </div>
        <div>
          <FieldLabel>Proxy URL</FieldLabel>
          <input
            className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
            value={run.proxyUrl}
            onChange={(e) => run.setProxyUrl(e.target.value)}
            placeholder="e.g. http://localhost:8120"
          />
        </div>
        <div className="grid grid-cols-2 gap-3">
          <div>
            <FieldLabel>Timeout (ms)</FieldLabel>
            <input
              className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
              value={run.timeoutMs}
              onChange={(e) => run.setTimeoutMs(e.target.value)}
              inputMode="numeric"
            />
          </div>
          <div>
            <FieldLabel>Max capture bytes</FieldLabel>
            <input
              className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
              value={run.maxCaptureBytes}
              onChange={(e) => run.setMaxCaptureBytes(e.target.value)}
              inputMode="numeric"
            />
          </div>
        </div>
        <div className="grid grid-cols-2 gap-3">
          <ToggleRow label="Stream assistant" checked={run.streamAssistant} onChange={run.setStreamAssistant} />
          <ToggleRow label="Trace" checked={run.trace} onChange={run.setTrace} />
        </div>
        <div className="grid grid-cols-2 gap-3">
          <ToggleRow label="YOLO (no tool restrictions)" checked={run.yolo} onChange={run.setYolo} />
          <ToggleRow label="Verbose" checked={run.verbose} onChange={run.setVerbose} />
          <ToggleRow label="Async run" checked={run.useAsync} onChange={run.setUseAsync} disabled={!jobsEnabled} />
          <ToggleRow
            label="Show debug in conversation"
            checked={client.showDebugInConversation}
            onChange={client.setShowDebugInConversation}
          />
        </div>
        {!jobsEnabled ? <div className="text-[11px] text-amber-200">Async run disabled by daemon caps.</div> : null}
      </div>
    </div>
  );
}
