import React from "react";
import type { SessionOperatorResp } from "../../api";

import {
  jsonText,
  serviceRefOf,
  serviceSummary,
  type SessionOperatorMutation,
  type SessionOperatorRow,
} from "./settingsBrokerSessionOperatorUtils";

type Props = {
  selectedServiceRef: string;
  setSelectedServiceRef: React.Dispatch<React.SetStateAction<string>>;
  serviceWaitMs: string;
  setServiceWaitMs: React.Dispatch<React.SetStateAction<string>>;
  serviceRecipe: string;
  setServiceRecipe: React.Dispatch<React.SetStateAction<string>>;
  serviceArgsJson: string;
  setServiceArgsJson: React.Dispatch<React.SetStateAction<string>>;
  serviceNotice: string | null;
  serviceRows: SessionOperatorRow[];
  serviceDetail: { data: SessionOperatorResp | undefined };
  attachService: SessionOperatorMutation;
  waitService: SessionOperatorMutation;
  runService: SessionOperatorMutation;
};

export default function SettingsBrokerSessionServicesSection(props: Props) {
  return (
    <div className="mt-4 rounded-md border border-white/10 bg-black/20 p-3" data-testid="session-services-section">
      <div className="text-xs font-semibold text-white/80">Services</div>
      <div className="mt-1 text-[11px] text-white/60">Service inspection and recipe execution stay scoped to the current session.</div>
      <div className="mt-3 grid grid-cols-2 gap-3">
        <div>
          <div className="text-[11px] text-white/60">Available services</div>
          <div className="mt-1 max-h-48 overflow-auto rounded-md border border-white/10 bg-black/30">
            {props.serviceRows.length === 0 ? <div className="px-3 py-2 text-[11px] text-white/50">No remote services reported.</div> : null}
            {props.serviceRows.map((row, idx) => {
              const ref = serviceRefOf(row) || `service-${idx}`;
              return (
                <button
                  key={`${ref}-${idx}`}
                  type="button"
                  className={`block w-full px-3 py-2 text-left text-[11px] hover:bg-white/5 ${props.selectedServiceRef === ref ? "bg-white/10" : ""}`}
                  onClick={() => props.setSelectedServiceRef(ref)}
                >
                  {serviceSummary(row)}
                </button>
              );
            })}
          </div>
        </div>
        <div>
          <div className="flex items-center justify-between gap-2">
            <div className="text-[11px] text-white/60">Service detail</div>
            <div className="flex items-center gap-2">
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                type="button"
                onClick={() => void props.attachService.mutateAsync().catch(() => {})}
                disabled={!props.selectedServiceRef || props.attachService.isPending}
              >
                Attach
              </button>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                type="button"
                onClick={() => void props.waitService.mutateAsync().catch(() => {})}
                disabled={!props.selectedServiceRef || props.waitService.isPending}
              >
                Wait
              </button>
            </div>
          </div>
          <pre className="mt-1 max-h-48 overflow-auto rounded-md border border-white/10 bg-black/30 p-2 text-[11px] text-white/80">{jsonText(props.serviceDetail.data || {})}</pre>
          <div className="mt-2 grid grid-cols-[120px,1fr] gap-2">
            <input
              className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
              value={props.serviceWaitMs}
              onChange={(e) => props.setServiceWaitMs(e.target.value)}
              placeholder="timeout ms"
            />
            <input
              className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
              value={props.serviceRecipe}
              onChange={(e) => props.setServiceRecipe(e.target.value)}
              placeholder="recipe"
            />
          </div>
          <textarea
            className="mt-2 min-h-[88px] w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
            value={props.serviceArgsJson}
            onChange={(e) => props.setServiceArgsJson(e.target.value)}
            placeholder='{"path":"/health"}'
          />
          <button
            className="mt-2 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            onClick={() => void props.runService.mutateAsync().catch(() => {})}
            disabled={!props.selectedServiceRef || !props.serviceRecipe.trim() || props.runService.isPending}
          >
            Run recipe
          </button>
          {props.serviceNotice ? <pre className="mt-2 max-h-28 overflow-auto rounded-md border border-white/10 bg-black/30 p-2 text-[11px] text-white/70">{props.serviceNotice}</pre> : null}
          {props.attachService.isError ? <div className="mt-1 text-[11px] text-rose-200">{String(props.attachService.error)}</div> : null}
          {props.waitService.isError ? <div className="mt-1 text-[11px] text-rose-200">{String(props.waitService.error)}</div> : null}
          {props.runService.isError ? <div className="mt-1 text-[11px] text-rose-200">{String(props.runService.error)}</div> : null}
        </div>
      </div>
    </div>
  );
}
