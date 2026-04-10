import React from "react";
import type { SessionOperatorResp } from "../../api";

import FieldLabel from "../FieldLabel";
import {
  jsonText,
  shellOutputText,
  shellRefOf,
  shellSummary,
  type SessionOperatorMutation,
  type SessionOperatorRow,
} from "./settingsBrokerSessionOperatorUtils";

type Props = {
  shellCommand: string;
  setShellCommand: React.Dispatch<React.SetStateAction<string>>;
  shellIntent: string;
  setShellIntent: React.Dispatch<React.SetStateAction<string>>;
  shellLabel: string;
  setShellLabel: React.Dispatch<React.SetStateAction<string>>;
  selectedShellRef: string;
  setSelectedShellRef: React.Dispatch<React.SetStateAction<string>>;
  shellInput: string;
  setShellInput: React.Dispatch<React.SetStateAction<string>>;
  shellNotice: string | null;
  shellRows: SessionOperatorRow[];
  shellDetail: { data: SessionOperatorResp | undefined };
  startShell: SessionOperatorMutation;
  pollShell: SessionOperatorMutation;
  sendShell: SessionOperatorMutation;
  terminateShell: SessionOperatorMutation;
};

export default function SettingsBrokerSessionShellsSection(props: Props) {
  return (
    <div className="mt-4 rounded-md border border-white/10 bg-black/20 p-3" data-testid="session-shells-section">
      <div className="text-xs font-semibold text-white/80">Shells</div>
      <div className="mt-1 text-[11px] text-white/60">Use shells for remote host examination. Keep transcript and live session events alongside shell output for correlation.</div>
      <div className="mt-3 grid grid-cols-1 gap-3">
        <div>
          <FieldLabel>Start shell</FieldLabel>
          <input
            className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
            data-testid="session-shell-command-input"
            value={props.shellCommand}
            onChange={(e) => props.setShellCommand(e.target.value)}
            placeholder="pwd"
          />
          <div className="mt-2 grid grid-cols-2 gap-2">
            <select
              className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
              value={props.shellIntent}
              onChange={(e) => props.setShellIntent(e.target.value)}
            >
              <option value="observation">observation</option>
              <option value="prerequisite">prerequisite</option>
              <option value="service">service</option>
            </select>
            <input
              className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
              value={props.shellLabel}
              onChange={(e) => props.setShellLabel(e.target.value)}
              placeholder="optional label"
            />
          </div>
          <button
            className="mt-2 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            onClick={() => void props.startShell.mutateAsync().catch(() => {})}
            disabled={props.startShell.isPending || !props.shellCommand.trim()}
          >
            {props.startShell.isPending ? "Starting…" : "Start shell"}
          </button>
          {props.startShell.isError ? <div className="mt-1 text-[11px] text-rose-200">{String(props.startShell.error)}</div> : null}
        </div>
        <div className="grid grid-cols-2 gap-3">
          <div>
            <div className="text-[11px] text-white/60">Active shells</div>
            <div className="mt-1 max-h-48 overflow-auto rounded-md border border-white/10 bg-black/30">
              {props.shellRows.length === 0 ? <div className="px-3 py-2 text-[11px] text-white/50">No remote shell jobs reported.</div> : null}
              {props.shellRows.map((row, idx) => {
                const ref = shellRefOf(row) || `shell-${idx}`;
                return (
                  <button
                    key={`${ref}-${idx}`}
                    type="button"
                    className={`block w-full px-3 py-2 text-left text-[11px] hover:bg-white/5 ${props.selectedShellRef === ref ? "bg-white/10" : ""}`}
                    onClick={() => props.setSelectedShellRef(ref)}
                  >
                    {shellSummary(row)}
                  </button>
                );
              })}
            </div>
          </div>
          <div>
            <div className="flex items-center justify-between gap-2">
              <div className="text-[11px] text-white/60">Shell detail</div>
              <div className="flex items-center gap-2">
                <button
                  className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                  type="button"
                  onClick={() => void props.pollShell.mutateAsync().catch(() => {})}
                  disabled={!props.selectedShellRef || props.pollShell.isPending}
                >
                  Poll
                </button>
                <button
                  className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                  type="button"
                  onClick={() => void props.terminateShell.mutateAsync().catch(() => {})}
                  disabled={!props.selectedShellRef || props.terminateShell.isPending}
                >
                  Terminate
                </button>
              </div>
            </div>
            <pre className="mt-1 max-h-48 overflow-auto rounded-md border border-white/10 bg-black/30 p-2 text-[11px] text-white/80">{jsonText(props.shellDetail.data || {})}</pre>
            {shellOutputText(props.shellDetail.data) ? (
              <>
                <div className="mt-2 text-[11px] text-white/60">Output</div>
                <pre className="mt-1 max-h-40 overflow-auto rounded-md border border-white/10 bg-black/30 p-2 text-[11px] text-white/80">{shellOutputText(props.shellDetail.data)}</pre>
              </>
            ) : null}
            <div className="mt-2 flex gap-2">
              <input
                className="min-w-0 flex-1 rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                value={props.shellInput}
                onChange={(e) => props.setShellInput(e.target.value)}
                placeholder="stdin text"
              />
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                type="button"
                onClick={() => void props.sendShell.mutateAsync().catch(() => {})}
                disabled={!props.selectedShellRef || !props.shellInput.trim() || props.sendShell.isPending}
              >
                Send
              </button>
            </div>
            {props.shellNotice ? <pre className="mt-2 max-h-28 overflow-auto rounded-md border border-white/10 bg-black/30 p-2 text-[11px] text-white/70">{props.shellNotice}</pre> : null}
            {props.pollShell.isError ? <div className="mt-1 text-[11px] text-rose-200">{String(props.pollShell.error)}</div> : null}
            {props.sendShell.isError ? <div className="mt-1 text-[11px] text-rose-200">{String(props.sendShell.error)}</div> : null}
            {props.terminateShell.isError ? <div className="mt-1 text-[11px] text-rose-200">{String(props.terminateShell.error)}</div> : null}
          </div>
        </div>
      </div>
    </div>
  );
}
