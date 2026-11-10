import React from "react";

export type AppHeaderProfile = { id: string; name: string };
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
    <header ref={props.topbarRef} className="workspace-topbar">
      <div className="workspace-brand"><div className="workspace-brand-mark" aria-hidden="true">a<span>c</span></div><div><div className="workspace-brand-name">agent<span> / C</span></div><div className="workspace-brand-caption">Agent operations</div></div></div>
      <div className="workspace-profile">
        <label htmlFor="workspace-profile">Environment</label>
        <select id="workspace-profile" value={props.activeProfileId} onChange={(e) => props.onProfileChange(e.target.value)} title={props.profileName}>
          {props.profiles.map((profile) => <option key={profile.id} value={profile.id}>{profile.name}</option>)}
        </select>
        {props.runOverridesEnabled ? <span className="workspace-overrides">Overrides</span> : null}
      </div>
      <div className="workspace-topbar-actions">
        <div className={`workspace-health health-${props.healthState}`} role="status" title={`${props.effectiveBase} · ${props.healthService ?? "agentd"} ${props.healthVersion ?? ""}`}><span />{props.healthState === "ok" ? "Connected" : props.healthState === "checking" ? "Connecting" : "Offline"}</div>
        <button className="workspace-button workspace-recheck" onClick={props.onRecheck} type="button">Recheck</button>
        <button className="workspace-button" onClick={props.onShowSettings} type="button">Settings</button>
      </div>
    </header>
  );
}
