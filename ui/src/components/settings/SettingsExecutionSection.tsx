import React from "react";
import type { Caps, DaemonConfigResp } from "../../api";
import type { ClientSettings, ConnectionSettings, RunSettings } from "../../hooks/useUiSettings";
import FieldLabel from "../FieldLabel";
import { SectionHeader, ToggleRow } from "./SettingsControls";

type SettingsExecutionSectionProps = {
  connection: ConnectionSettings;
  run: RunSettings;
  client: ClientSettings;
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
  caps: {
    data?: Caps;
    source: "live" | "cache" | "none";
    isFetching: boolean;
    error: string | null;
    refresh: () => void;
  };
  capsAge: string;
  capsJson: string;
  connectorStaleMinutes: string;
  setConnectorStaleMinutes: (next: string) => void;
  jobsEnabled: boolean;
  baseUrlLabel: string;
  fetchOpenRouterModelsPending: boolean;
  fetchOpenRouterModelsError: string | null;
  onFetchOpenRouterModels: () => void;
  openrouterModels: any | null;
};

export default function SettingsExecutionSection(props: SettingsExecutionSectionProps) {
  const {
    connection,
    run,
    client,
    daemonConfig,
    updateDaemonDefaults,
    daemonDefaults,
    caps,
    capsAge,
    capsJson,
    connectorStaleMinutes,
    setConnectorStaleMinutes,
    jobsEnabled,
    baseUrlLabel,
    fetchOpenRouterModelsPending,
    fetchOpenRouterModelsError,
    onFetchOpenRouterModels,
    openrouterModels,
  } = props;

  return (
    <>
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

      <details className="mt-4 rounded-md border border-white/10 bg-black/20 p-3">
        <summary className="cursor-pointer text-xs font-semibold text-white/70">Run limits</summary>
        <div className="mt-3 grid gap-3 text-[11px] text-white/70">
          <div className="grid grid-cols-2 gap-3">
            <div>
              <FieldLabel>Max steps</FieldLabel>
              <input
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                value={run.maxSteps}
                onChange={(e) => run.setMaxSteps(e.target.value)}
                placeholder="blank = daemon default"
              />
            </div>
            <div>
              <FieldLabel>Max repeated tool calls</FieldLabel>
              <input
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                value={run.maxRepeatedToolCalls}
                onChange={(e) => run.setMaxRepeatedToolCalls(e.target.value)}
                inputMode="numeric"
              />
            </div>
          </div>
          <div className="grid grid-cols-2 gap-3">
            <div>
              <FieldLabel>Max tool calls total</FieldLabel>
              <input
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                value={run.maxToolCallsTotal}
                onChange={(e) => run.setMaxToolCallsTotal(e.target.value)}
                placeholder="blank = daemon default"
              />
            </div>
            <div>
              <FieldLabel>Max tool calls per tool</FieldLabel>
              <input
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                value={run.maxToolCallsPerTool}
                onChange={(e) => run.setMaxToolCallsPerTool(e.target.value)}
                placeholder="blank = daemon default"
              />
            </div>
          </div>
          <div>
            <FieldLabel>Tool call limits</FieldLabel>
            <textarea
              className="mt-1 min-h-[90px] w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
              value={run.toolCallLimits}
              onChange={(e) => run.setToolCallLimits(e.target.value)}
              placeholder="tool=max_calls (comma or newline separated) or JSON list"
            />
          </div>
          <div className="grid grid-cols-2 gap-3">
            <div>
              <FieldLabel>Max chars</FieldLabel>
              <input
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                value={run.maxChars}
                onChange={(e) => run.setMaxChars(e.target.value)}
                inputMode="numeric"
              />
            </div>
            <div>
              <FieldLabel>Keep last</FieldLabel>
              <input
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                value={run.keepLast}
                onChange={(e) => run.setKeepLast(e.target.value)}
                inputMode="numeric"
              />
            </div>
          </div>
        </div>
      </details>

      <details className="mt-4 rounded-md border border-white/10 bg-black/20 p-3">
        <summary className="cursor-pointer text-xs font-semibold text-white/70">Memory context</summary>
        <div className="mt-3 grid gap-3 text-[11px] text-white/70">
          <div>
            <FieldLabel>Context mode</FieldLabel>
            <select
              className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
              value={run.memoryContextMode}
              onChange={(e) => run.setMemoryContextMode(e.target.value)}
            >
              <option value="files">files (read memory/*.md)</option>
              <option value="index">index (progressive file index)</option>
              <option value="search">search (ranked snippets)</option>
              <option value="salience">salience (ranked recency/importance)</option>
            </select>
            <div className="mt-1 text-[11px] text-white/40">
              Applies only when tools=host; index shows file size/line/token estimates; search defaults to the prompt if no query is provided.
            </div>
          </div>
          <div className="grid grid-cols-2 gap-3">
            <ToggleRow label="Include structured" checked={run.memoryIncludeStructured} onChange={run.setMemoryIncludeStructured} />
            <ToggleRow label="Include core" checked={run.memoryIncludeCore} onChange={run.setMemoryIncludeCore} />
            <ToggleRow label="Include daily" checked={run.memoryIncludeDaily} onChange={run.setMemoryIncludeDaily} />
            <ToggleRow label="Include session" checked={run.memoryIncludeSession} onChange={run.setMemoryIncludeSession} />
          </div>
          <div className="grid grid-cols-2 gap-3">
            <div>
              <FieldLabel>Daily days</FieldLabel>
              <input
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                value={run.memoryDailyDays}
                onChange={(e) => run.setMemoryDailyDays(e.target.value)}
                inputMode="numeric"
              />
            </div>
            <div>
              <FieldLabel>Total cap (bytes)</FieldLabel>
              <input
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                value={run.memoryTotalCap}
                onChange={(e) => run.setMemoryTotalCap(e.target.value)}
                inputMode="numeric"
              />
            </div>
          </div>
          <div>
            <FieldLabel>Search query (optional)</FieldLabel>
            <textarea
              className="mt-1 min-h-[70px] w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
              value={run.memorySearchQuery}
              onChange={(e) => run.setMemorySearchQuery(e.target.value)}
              placeholder="Leave blank to use the current prompt"
            />
          </div>
          <div className="grid grid-cols-2 gap-3">
            <ToggleRow label="Use index (FTS)" checked={run.memorySearchUseIndex} onChange={run.setMemorySearchUseIndex} />
            <ToggleRow label="Case sensitive" checked={run.memorySearchCaseSensitive} onChange={run.setMemorySearchCaseSensitive} />
            <ToggleRow
              label="Fallback to files"
              checked={run.memorySearchFallbackToFiles}
              onChange={run.setMemorySearchFallbackToFiles}
            />
          </div>
          <div>
            <FieldLabel>Order</FieldLabel>
            <select
              className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
              value={run.memorySearchOrder}
              onChange={(e) => run.setMemorySearchOrder(e.target.value)}
            >
              <option value="ranked">Ranked (relevance)</option>
              <option value="newest">Newest first</option>
              <option value="oldest">Oldest first</option>
            </select>
          </div>
          <div className="grid grid-cols-3 gap-3">
            <div>
              <FieldLabel>Max results</FieldLabel>
              <input
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                value={run.memorySearchMaxResults}
                onChange={(e) => run.setMemorySearchMaxResults(e.target.value)}
                inputMode="numeric"
              />
            </div>
            <div>
              <FieldLabel>Max snippet chars</FieldLabel>
              <input
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                value={run.memorySearchMaxSnippetChars}
                onChange={(e) => run.setMemorySearchMaxSnippetChars(e.target.value)}
                inputMode="numeric"
              />
            </div>
            <div>
              <FieldLabel>Context lines</FieldLabel>
              <input
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                value={run.memorySearchContextLines}
                onChange={(e) => run.setMemorySearchContextLines(e.target.value)}
                inputMode="numeric"
              />
            </div>
          </div>
        </div>
      </details>

      <details className="mt-4 rounded-md border border-white/10 bg-black/20 p-3">
        <summary className="cursor-pointer text-xs font-semibold text-white/70">OpenRouter model picker</summary>
        <div className="mt-3 grid gap-3 text-[11px] text-white/70">
          <div className="grid grid-cols-2 gap-3">
            <div>
              <FieldLabel>Min total ($/1M)</FieldLabel>
              <input
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                value={run.orMinTotal}
                onChange={(e) => run.setOrMinTotal(e.target.value)}
              />
            </div>
            <div>
              <FieldLabel>Max total ($/1M)</FieldLabel>
              <input
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                value={run.orMaxTotal}
                onChange={(e) => run.setOrMaxTotal(e.target.value)}
              />
            </div>
            <div>
              <FieldLabel>Limit</FieldLabel>
              <input
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                value={run.orLimit}
                onChange={(e) => run.setOrLimit(e.target.value)}
              />
            </div>
            <div className="grid gap-2">
              <ToggleRow
                label="Require multimodal"
                checked={run.orRequireMultimodal}
                onChange={run.setOrRequireMultimodal}
              />
              <ToggleRow label="Require tools" checked={run.orRequireTools} onChange={run.setOrRequireTools} />
            </div>
          </div>

          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={fetchOpenRouterModelsPending}
            onClick={onFetchOpenRouterModels}
          >
            {fetchOpenRouterModelsPending ? "Loading…" : "Fetch models"}
          </button>
          {fetchOpenRouterModelsError ? (
            <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-rose-200">
              Fetch failed: {fetchOpenRouterModelsError}
            </div>
          ) : null}

          {openrouterModels ? (
            <div className="rounded-md border border-white/10 bg-black/30 p-3">
              <div className="text-white/60">
                {openrouterModels.ok ? (
                  <>
                    <span className="text-emerald-200">ok</span> · count={openrouterModels.count ?? 0} ·
                    cached={String(openrouterModels.cached ?? false)}
                  </>
                ) : (
                  <span className="text-rose-200">error</span>
                )}
              </div>
              {openrouterModels.error ? <div className="mt-2 text-rose-200">{String(openrouterModels.error)}</div> : null}
              {openrouterModels.recommended_model ? (
                <div className="mt-2">
                  Recommended: <code className="text-white/80">{openrouterModels.recommended_model}</code>
                </div>
              ) : null}
              {Array.isArray(openrouterModels.models) && openrouterModels.models.length > 0 ? (
                <div className="mt-3 max-h-48 overflow-auto rounded-md border border-white/10">
                  {openrouterModels.models.slice(0, 50).map((m: any) => {
                    const id = typeof m?.id === "string" ? m.id : "";
                    if (!id) return null;
                    const total = typeof m?.total_usd_per_million === "number" ? m.total_usd_per_million : null;
                    const ctx = typeof m?.context_length === "number" ? m.context_length : null;
                    return (
                      <button
                        key={id}
                        type="button"
                        className="flex w-full items-center justify-between gap-2 px-3 py-2 text-left text-[11px] hover:bg-white/5"
                        onClick={() => {
                          run.setModel(id);
                          run.setBaseUrl("https://openrouter.ai/api/v1");
                        }}
                      >
                        <span className="font-mono text-white/80">{id}</span>
                        <span className="text-white/50">
                          {ctx ? `ctx=${ctx}` : ""}
                          {total !== null ? ` · $${total.toFixed(3)}/1M` : ""}
                        </span>
                      </button>
                    );
                  })}
                </div>
              ) : null}
            </div>
          ) : null}
        </div>
      </details>

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
              max_tool_calls_total_default: <code className="text-white/70">{String(daemonDefaults.max_tool_calls_total_default ?? "(unset)")}</code>
            </div>
            <div>
              max_tool_calls_per_tool_default: <code className="text-white/70">{String(daemonDefaults.max_tool_calls_per_tool_default ?? "(unset)")}</code>
            </div>
            <div>
              max_tool_call_args_chars_default: <code className="text-white/70">{String(daemonDefaults.max_tool_call_args_chars_default ?? "(unset)")}</code>
            </div>
          </div>
        ) : null}
      </div>

      <details className="mt-4 rounded-md border border-white/10 bg-black/20 p-3">
        <summary className="cursor-pointer text-xs font-semibold text-white/70">Capabilities</summary>
        <div className="mt-2 grid gap-2 text-[11px] text-white/70">
          <div className="flex flex-wrap items-center justify-between gap-2">
            <div>
              service: <code className="text-white/70">{caps.data?.service || "(unknown)"}</code> · version:{" "}
              <code className="text-white/70">{caps.data?.version || "(unknown)"}</code>
            </div>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
              type="button"
              onClick={() => caps.refresh()}
              disabled={caps.isFetching}
            >
              {caps.isFetching ? "Loading…" : "Refresh"}
            </button>
          </div>
          <div>
            source: <code className="text-white/70">{caps.source}</code>
            {capsAge ? <span className="text-white/50"> · age {capsAge}</span> : null}
          </div>
          {caps.error ? (
            <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-rose-200">
              caps fetch failed: {String(caps.error)}
            </div>
          ) : null}
          {capsJson ? (
            <pre className="max-h-64 overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/30 p-2 text-[10px] text-white/60">
              {capsJson}
            </pre>
          ) : null}
        </div>
      </details>
    </>
  );
}
