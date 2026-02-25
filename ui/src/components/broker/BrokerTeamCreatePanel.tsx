import React from "react";
import FieldLabel from "../FieldLabel";

type BrokerTeamCreatePanelProps = {
  canQuery: boolean;
  teamsBusy: boolean;
  newTeamId: string;
  newTeamName: string;
  onNewTeamIdChange: (next: string) => void;
  onNewTeamNameChange: (next: string) => void;
  onCreateTeam: () => void;
};

export default function BrokerTeamCreatePanel(props: BrokerTeamCreatePanelProps) {
  return (
    <div className="mt-3 grid gap-2 rounded-md border border-white/10 bg-black/30 p-2">
      <div className="text-xs font-semibold text-white/80">Create team</div>
      <div className="flex flex-wrap items-center gap-2">
        <FieldLabel>ID</FieldLabel>
        <input
          className="min-w-[180px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
          value={props.newTeamId}
          onChange={(e) => props.onNewTeamIdChange(e.target.value)}
          placeholder="team-ops"
        />
        <FieldLabel>Name</FieldLabel>
        <input
          className="min-w-[200px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
          value={props.newTeamName}
          onChange={(e) => props.onNewTeamNameChange(e.target.value)}
          placeholder="Ops team"
        />
        <button
          className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
          type="button"
          disabled={!props.canQuery || props.teamsBusy}
          onClick={props.onCreateTeam}
        >
          Create
        </button>
      </div>
    </div>
  );
}
