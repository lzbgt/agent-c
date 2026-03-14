import React from "react";
import type { Caps } from "../../api";

type SettingsCapabilitiesSectionProps = {
  caps: {
    data?: Caps;
    source: "live" | "cache" | "none";
    isFetching: boolean;
    error: string | null;
    refresh: () => void;
  };
  capsAge: string;
  capsJson: string;
};

export default function SettingsCapabilitiesSection(props: SettingsCapabilitiesSectionProps) {
  const { caps, capsAge, capsJson } = props;

  return (
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
  );
}
