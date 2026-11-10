import React from "react";
import { useDeploymentIdentity } from "../DeploymentAuthBoundary";
import WorkspaceIcon from "./WorkspaceIcon";

type AppHeaderProps = {
  effectiveBase: string;
  healthState: "ok" | "checking" | "offline";
  healthService?: string;
  healthVersion?: string;
  onShowSettings: () => void;
};

export default function AppHeader(props: AppHeaderProps) {
  const identity = useDeploymentIdentity();
  return (
    <header className="workspace-topbar">
      <div className="workspace-brand"><div className="workspace-brand-mark" aria-hidden="true">a<span>c</span></div><div><div className="workspace-brand-name">agent<span> / C</span></div><div className="workspace-brand-caption">Agent operations</div></div></div>
      <div className="workspace-topbar-actions">
        <div className={`workspace-health health-${props.healthState}`} role="status" aria-label={props.healthState === "ok" ? "Connected" : props.healthState === "checking" ? "Connecting" : "Offline"} title={`${props.effectiveBase} · ${props.healthService ?? "agentd"} ${props.healthVersion ?? ""}`}><span /><span className="workspace-health-label">{props.healthState === "ok" ? "Connected" : props.healthState === "checking" ? "Connecting" : "Offline"}</span></div>
        <button className="workspace-button workspace-header-action" aria-label="Settings" title="Settings" onClick={props.onShowSettings} type="button"><WorkspaceIcon name="settings" /><span>Settings</span></button>
        {identity ? <button className="workspace-button workspace-header-action" title={`Sign out · ${identity.displayName}`} aria-label="Sign out of workspace" disabled={identity.signingOut} onClick={() => void identity.signOut()} type="button"><WorkspaceIcon name="sign-out" /><span>Sign out</span></button> : null}
      </div>
    </header>
  );
}
