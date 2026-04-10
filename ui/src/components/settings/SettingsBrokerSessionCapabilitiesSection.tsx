import React from "react";
import type { SessionOperatorResp } from "../../api";

import { capabilityRefOf, capabilitySummary, jsonText, type SessionOperatorRow } from "./settingsBrokerSessionOperatorUtils";

type Props = {
  selectedCapabilityRef: string;
  setSelectedCapabilityRef: React.Dispatch<React.SetStateAction<string>>;
  capabilityRows: SessionOperatorRow[];
  capabilityDetail: { data: SessionOperatorResp | undefined };
};

export default function SettingsBrokerSessionCapabilitiesSection(props: Props) {
  return (
    <div className="mt-4 rounded-md border border-white/10 bg-black/20 p-3" data-testid="session-capabilities-section">
      <div className="text-xs font-semibold text-white/80">Capabilities</div>
      <div className="mt-1 text-[11px] text-white/60">Use capabilities to explain what the remote session can do before inventing new synthetic controls.</div>
      <div className="mt-3 grid grid-cols-2 gap-3">
        <div>
          <div className="text-[11px] text-white/60">Available capabilities</div>
          <div className="mt-1 max-h-40 overflow-auto rounded-md border border-white/10 bg-black/30">
            {props.capabilityRows.length === 0 ? <div className="px-3 py-2 text-[11px] text-white/50">No capabilities reported.</div> : null}
            {props.capabilityRows.map((row, idx) => {
              const ref = capabilityRefOf(row) || `cap-${idx}`;
              return (
                <button
                  key={`${ref}-${idx}`}
                  type="button"
                  className={`block w-full px-3 py-2 text-left text-[11px] hover:bg-white/5 ${props.selectedCapabilityRef === ref ? "bg-white/10" : ""}`}
                  onClick={() => props.setSelectedCapabilityRef(ref)}
                >
                  {capabilitySummary(row)}
                </button>
              );
            })}
          </div>
        </div>
        <div>
          <div className="text-[11px] text-white/60">Capability detail</div>
          <pre className="mt-1 max-h-40 overflow-auto rounded-md border border-white/10 bg-black/30 p-2 text-[11px] text-white/80">{jsonText(props.capabilityDetail.data || {})}</pre>
        </div>
      </div>
    </div>
  );
}
