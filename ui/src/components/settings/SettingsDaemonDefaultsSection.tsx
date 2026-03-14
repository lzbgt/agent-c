import React from "react";
import type { DaemonConfigResp } from "../../api";
import { SectionHeader } from "./SettingsControls";

type SettingsDaemonDefaultsSectionProps = {
  daemonConfig: {
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
  daemonDefaults?: DaemonConfigResp["daemon"];
};

export default function SettingsDaemonDefaultsSection(props: SettingsDaemonDefaultsSectionProps) {
  const { daemonConfig, updateDaemonDefaults, daemonDefaults } = props;

  return (
    <div className="mt-4 rounded-md border border-white/10 bg-black/20 p-3">
      <SectionHeader
        title="Daemon defaults (persisted)"
        action={
          <button
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            onClick={() => daemonConfig.refresh()}
            type="button"
            disabled={daemonConfig.isFetching}
          >
            Refresh
          </button>
        }
      />
      <div className="mt-2 text-[11px] text-white/60">
        Saves to daemon state (server-side). This avoids keeping provider keys in browser storage.
      </div>
      <div className="mt-3 grid gap-2">
        <button
          className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50"
          type="button"
          disabled={updateDaemonDefaults.pending}
          onClick={updateDaemonDefaults.saveDefaults}
        >
          Save model/base_url/proxy/timeout to daemon
        </button>
        <div className="flex items-center gap-2">
          <button
            className="flex-1 rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={updateDaemonDefaults.pending}
            onClick={updateDaemonDefaults.saveApiKey}
            title="Stores the provider key on the daemon host (in state_dir/runtime_secrets.env)."
          >
            Save API key to daemon (current provider)
          </button>
          <button
            className="rounded-md border border-rose-500/30 bg-rose-500/10 px-3 py-2 text-xs text-rose-200 hover:bg-rose-500/15 disabled:opacity-50"
            type="button"
            disabled={updateDaemonDefaults.pending}
            onClick={updateDaemonDefaults.clearApiKey}
          >
            Clear key
          </button>
        </div>
        {updateDaemonDefaults.error ? (
          <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-3 py-2 text-xs text-rose-200">
            Save failed: {updateDaemonDefaults.error}
          </div>
        ) : null}
        {updateDaemonDefaults.success ? (
          <div className="rounded-md border border-emerald-500/30 bg-emerald-500/10 px-3 py-2 text-xs text-emerald-100">
            Saved.
          </div>
        ) : null}
      </div>
      {daemonDefaults ? (
        <div className="mt-3 grid gap-1 text-[11px] text-white/60">
          <div>base_url: <code className="text-white/70">{daemonDefaults.base_url || "(unset)"}</code></div>
          <div>model: <code className="text-white/70">{daemonDefaults.model || "(unset)"}</code></div>
          <div>summary_model: <code className="text-white/70">{daemonDefaults.summary_model || "(unset)"}</code></div>
          <div>summary_max_chars: <code className="text-white/70">{String(daemonDefaults.summary_max_chars ?? "(unset)")}</code></div>
          <div>timeout_ms: <code className="text-white/70">{String(daemonDefaults.timeout_ms ?? "(unset)")}</code></div>
          <div>proxy_url_set: <code className="text-white/70">{String(daemonDefaults.proxy_url_set ?? false)}</code></div>
          <div>api_key_set: <code className="text-white/70">{String(daemonDefaults.api_key_set ?? false)}</code></div>
          <div>max_steps_default: <code className="text-white/70">{String(daemonDefaults.max_steps_default ?? "(unset)")}</code></div>
          <div>
            max_tool_calls_total_default:{" "}
            <code className="text-white/70">{String(daemonDefaults.max_tool_calls_total_default ?? "(unset)")}</code>
          </div>
          <div>
            max_tool_calls_per_tool_default:{" "}
            <code className="text-white/70">{String(daemonDefaults.max_tool_calls_per_tool_default ?? "(unset)")}</code>
          </div>
          <div>
            max_tool_call_args_chars_default:{" "}
            <code className="text-white/70">{String(daemonDefaults.max_tool_call_args_chars_default ?? "(unset)")}</code>
          </div>
        </div>
      ) : null}
    </div>
  );
}
