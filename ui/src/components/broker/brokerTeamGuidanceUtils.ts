import type { BrokerGuidanceEvent } from "../../api";
import { asFiniteNumber, asUnknownRecord, type UnknownRecord } from "./brokerObjectUtils";

export type BrokerGuidanceBriefing = {
  goal?: string;
  proposed?: UnknownRecord;
  drift?: {
    elapsed_ms?: number;
    threshold_ms?: number;
    detected_unix_ms?: number;
  };
  team_run_id?: string;
  team_run_status?: string;
  team_run_elapsed_ms?: number;
  goal_contract?: UnknownRecord;
  role_plan_snapshot?: UnknownRecord;
};

export const normalizeGuidanceList = (rows?: BrokerGuidanceEvent[]) =>
  Array.isArray(rows) ? rows.filter((row) => row && typeof row === "object") : [];

export const getGuidanceBriefing = (payload: unknown): BrokerGuidanceBriefing | null => {
  const payloadRecord = asUnknownRecord(payload);
  const briefingRecord = asUnknownRecord(payloadRecord?.briefing);
  if (!briefingRecord) return null;
  const driftRecord = asUnknownRecord(briefingRecord.drift);
  return {
    goal: typeof briefingRecord.goal === "string" ? briefingRecord.goal : undefined,
    proposed: asUnknownRecord(briefingRecord.proposed) ?? undefined,
    drift: driftRecord
      ? {
          elapsed_ms: asFiniteNumber(driftRecord.elapsed_ms),
          threshold_ms: asFiniteNumber(driftRecord.threshold_ms),
          detected_unix_ms: asFiniteNumber(driftRecord.detected_unix_ms),
        }
      : undefined,
    team_run_id: typeof briefingRecord.team_run_id === "string" ? briefingRecord.team_run_id : undefined,
    team_run_status: typeof briefingRecord.team_run_status === "string" ? briefingRecord.team_run_status : undefined,
    team_run_elapsed_ms: asFiniteNumber(briefingRecord.team_run_elapsed_ms),
    goal_contract: asUnknownRecord(briefingRecord.goal_contract) ?? undefined,
    role_plan_snapshot: asUnknownRecord(briefingRecord.role_plan_snapshot) ?? undefined,
  };
};

export const formatDuration = (ms?: number | null) => {
  if (!ms || !Number.isFinite(ms)) return "";
  if (ms < 1000) return `${Math.round(ms)}ms`;
  const sec = ms / 1000;
  if (sec < 60) return `${sec.toFixed(1)}s`;
  const min = sec / 60;
  if (min < 60) return `${min.toFixed(1)}m`;
  const hr = min / 60;
  return `${hr.toFixed(1)}h`;
};
