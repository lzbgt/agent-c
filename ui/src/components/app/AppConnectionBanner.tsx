import React from "react";

type Props = { healthError: boolean; sessionsUnauthorized: boolean; onShowSettings: () => void };

export default function AppConnectionBanner(props: Props) {
  if (!props.healthError && !props.sessionsUnauthorized) return null;
  return <div role="status" className="flex flex-wrap items-center gap-3 border-b border-amber-500/20 bg-amber-500/10 px-4 py-2 text-xs text-amber-100">
    <span>{props.sessionsUnauthorized ? "Connection access needs attention." : "Connection unavailable."}</span>
    <button className="underline hover:text-white" onClick={props.onShowSettings} type="button">Connection settings</button>
  </div>;
}
