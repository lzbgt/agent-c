import React from "react";
import type { OpenRouterModelsResp } from "../../api";
import type { RunSettings } from "../../hooks/uiSettingsTypes";
import FieldLabel from "../FieldLabel";
import { ToggleRow } from "./SettingsControls";

type SettingsOpenRouterSectionProps = {
  run: RunSettings;
  fetchOpenRouterModelsPending: boolean;
  fetchOpenRouterModelsError: string | null;
  onFetchOpenRouterModels: () => void;
  openrouterModels: OpenRouterModelsResp | null;
};

export default function SettingsOpenRouterSection(props: SettingsOpenRouterSectionProps) {
  const { run, fetchOpenRouterModelsPending, fetchOpenRouterModelsError, onFetchOpenRouterModels, openrouterModels } = props;

  return (
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
                {openrouterModels.models.slice(0, 50).map((model) => {
                  const id = typeof model.id === "string" ? model.id : "";
                  if (!id) return null;
                  const total = typeof model.total_usd_per_million === "number" ? model.total_usd_per_million : null;
                  const ctx = typeof model.context_length === "number" ? model.context_length : null;
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
  );
}
