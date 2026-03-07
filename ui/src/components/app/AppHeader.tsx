import React from "react";

export type AppHeaderProfile = {
  id: string;
  name: string;
};

type AppHeaderProps = {
  topbarRef?: React.Ref<HTMLElement>;
  profiles: AppHeaderProfile[];
  activeProfileId: string;
  profileName: string;
  onProfileChange: (next: string) => void;
  runOverridesEnabled: boolean;
  effectiveBase: string;
  healthState: "ok" | "checking" | "offline";
  healthService?: string;
  healthVersion?: string;
  onRecheck: () => void;
  onShowSettings: () => void;
};

export default function AppHeader(props: AppHeaderProps) {
  return (
    <header ref={props.topbarRef} className="sticky top-0 z-30 border-b border-white/10 bg-slate-950/80 backdrop-blur">
      <div className="flex h-14 min-w-0 items-center justify-between px-4">
        <div className="min-w-0">
          <div className="text-sm font-semibold">agent UI</div>
          <div className="text-[11px] text-white/60">
            profile:{" "}
            <select
              className="ml-1 inline-block max-w-[28vw] truncate rounded-md border border-white/10 bg-black/30 px-2 py-0.5 font-mono text-[11px] text-white/70"
              value={props.activeProfileId}
              onChange={(e) => props.onProfileChange(e.target.value)}
              title={props.profileName}
            >
              {props.profiles.map((profile) => (
                <option key={profile.id} value={profile.id}>
                  {profile.name}
                </option>
              ))}
            </select>{" "}
            {props.runOverridesEnabled ? (
              <span className="rounded-full border border-emerald-400/40 bg-emerald-500/10 px-2 py-0.5 text-[10px] font-semibold text-emerald-200">
                run overrides
              </span>
            ) : null}
            · daemon:{" "}
            <span className="inline-block max-w-[60vw] truncate align-bottom font-mono text-[11px] text-white/70" title={props.effectiveBase}>
              {props.effectiveBase}
            </span>{" "}
            {props.healthState === "ok" ? (
              <span className="text-emerald-300">
                ok ({props.healthService ?? "agentd"} {props.healthVersion ?? ""})
              </span>
            ) : props.healthState === "checking" ? (
              <span className="text-white/60">checking…</span>
            ) : (
              <span className="text-rose-300">offline</span>
            )}
          </div>
        </div>
        <div className="flex items-center gap-2">
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40"
            onClick={props.onRecheck}
            type="button"
          >
            Recheck
          </button>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40"
            onClick={props.onShowSettings}
            type="button"
          >
            Settings
          </button>
        </div>
      </div>
    </header>
  );
}
