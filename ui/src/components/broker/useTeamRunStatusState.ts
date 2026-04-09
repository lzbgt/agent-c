import React from "react";
import {
  apiBrokerTeamRunGoalUpdate,
  apiBrokerTeamRunHandoff,
  type ApiAuth,
} from "../../api";
import type {
  TeamRunGoalContractRow,
  TeamRunGoalEventInput,
  TeamRunGoalEventRow,
  TeamRunGoalEventType,
  TeamRunHandoffEventInput,
  TeamRunHandoffEventRecord,
  TeamRunHandoffEventRow,
  TeamRunHandoffKind,
  TeamRunHandoffState,
  TeamRunHandoffTransitionState,
  TeamRunLookupResult,
} from "./teamRunStatusTypes";
import { normalizeHandoffEventRecord, parseLineList } from "./teamRunStatusUtils";

type UseTeamRunStatusStateArgs = {
  base: string;
  auth: ApiAuth;
  canWrite: boolean;
  teamId: string;
  runId: string;
  run: TeamRunLookupResult | null;
  handoffEvents: TeamRunHandoffEventRow[];
  onRefreshRun: (runId: string) => Promise<void> | void;
};

export default function useTeamRunStatusState(args: UseTeamRunStatusStateArgs) {
  const goalContract = args.run?.goal_contract ?? null;
  const goalEvents: TeamRunGoalEventRow[] = args.run?.goal_events ?? [];
  const lastInitRunId = React.useRef<string>("");

  const [goalContractGoal, setGoalContractGoal] = React.useState<string>("");
  const [goalContractCriteria, setGoalContractCriteria] = React.useState<string>("");
  const [goalContractConstraints, setGoalContractConstraints] = React.useState<string>("");
  const [goalEventType, setGoalEventType] = React.useState<string>("progress");
  const [goalEventMessage, setGoalEventMessage] = React.useState<string>("");
  const [goalEventData, setGoalEventData] = React.useState<string>("");
  const [goalUpdateBusy, setGoalUpdateBusy] = React.useState<boolean>(false);
  const [goalUpdateError, setGoalUpdateError] = React.useState<string | null>(null);
  const [goalUpdateNote, setGoalUpdateNote] = React.useState<string | null>(null);

  const [handoffFromRole, setHandoffFromRole] = React.useState<string>("");
  const [handoffToRole, setHandoffToRole] = React.useState<string>("");
  const [handoffKind, setHandoffKind] = React.useState<string>("role");
  const [handoffReason, setHandoffReason] = React.useState<string>("");
  const [handoffMessage, setHandoffMessage] = React.useState<string>("");
  const [handoffData, setHandoffData] = React.useState<string>("");
  const [handoffSourceDeployment, setHandoffSourceDeployment] = React.useState<string>("");
  const [handoffSourceSession, setHandoffSourceSession] = React.useState<string>("");
  const [handoffTargetDeployment, setHandoffTargetDeployment] = React.useState<string>("");
  const [handoffTargetSession, setHandoffTargetSession] = React.useState<string>("");
  const [handoffBusy, setHandoffBusy] = React.useState<boolean>(false);
  const [handoffError, setHandoffError] = React.useState<string | null>(null);
  const [handoffNote, setHandoffNote] = React.useState<string | null>(null);

  React.useEffect(() => {
    if (!args.runId) return;
    if (lastInitRunId.current === args.runId) return;
    lastInitRunId.current = args.runId;
    const goal = goalContract && typeof goalContract.goal === "string" ? goalContract.goal : "";
    const criteria = Array.isArray(goalContract?.success_criteria) ? goalContract.success_criteria : [];
    const constraints = Array.isArray(goalContract?.constraints) ? goalContract.constraints : [];
    setGoalContractGoal(goal);
    setGoalContractCriteria(criteria.join("\n"));
    setGoalContractConstraints(constraints.join("\n"));
    setGoalUpdateError(null);
    setGoalUpdateNote(null);
    setHandoffError(null);
    setHandoffNote(null);
  }, [args.runId, goalContract]);

  const handoffLatestById = React.useMemo(() => {
    const latest = new Map<string, TeamRunHandoffEventRecord>();
    for (const raw of args.handoffEvents) {
      const ev = normalizeHandoffEventRecord(raw);
      const hid = String(ev.handoff_id || "").trim();
      if (!hid) continue;
      latest.set(hid, ev);
    }
    return latest;
  }, [args.handoffEvents]);

  const handleGoalContractUpdate = async () => {
    setGoalUpdateError(null);
    setGoalUpdateNote(null);
    if (!args.canWrite) {
      setGoalUpdateError("missing team or run id");
      return;
    }
    const goal = goalContractGoal.trim();
    const criteria = parseLineList(goalContractCriteria);
    const constraints = parseLineList(goalContractConstraints);
    if (!goal && criteria.length === 0 && constraints.length === 0) {
      setGoalUpdateError("goal contract is empty");
      return;
    }
    const contract: TeamRunGoalContractRow = {};
    if (goal) contract.goal = goal;
    if (criteria.length > 0) contract.success_criteria = criteria;
    if (constraints.length > 0) contract.constraints = constraints;
    setGoalUpdateBusy(true);
    try {
      const resp = await apiBrokerTeamRunGoalUpdate(args.base, args.teamId, args.runId, { goal_contract: contract }, args.auth);
      if (!resp.ok) {
        setGoalUpdateError(resp.error || resp.err || "goal update failed");
        return;
      }
      setGoalUpdateNote("goal contract updated");
      await args.onRefreshRun(args.runId);
    } catch (err) {
      setGoalUpdateError(String(err));
    } finally {
      setGoalUpdateBusy(false);
    }
  };

  const handleGoalEvent = async () => {
    setGoalUpdateError(null);
    setGoalUpdateNote(null);
    if (!args.canWrite) {
      setGoalUpdateError("missing team or run id");
      return;
    }
    const eventType = String(goalEventType || "").trim().toLowerCase();
    if (eventType !== "progress" && eventType !== "drift" && eventType !== "spawn_validation") {
      setGoalUpdateError("goal event type must be progress, drift, or spawn_validation");
      return;
    }
    let dataObj: Record<string, unknown> | undefined;
    const rawData = goalEventData.trim();
    if (rawData) {
      try {
        const parsed = JSON.parse(rawData) as unknown;
        if (!parsed || typeof parsed !== "object" || Array.isArray(parsed)) {
          setGoalUpdateError("goal event data must be a JSON object");
          return;
        }
        dataObj = parsed as Record<string, unknown>;
      } catch (err) {
        setGoalUpdateError(`goal event data invalid json: ${String(err)}`);
        return;
      }
    }
    const event: TeamRunGoalEventInput = { type: eventType as TeamRunGoalEventType };
    const message = goalEventMessage.trim();
    if (message) event.message = message;
    if (dataObj) event.data = dataObj;
    setGoalUpdateBusy(true);
    try {
      const resp = await apiBrokerTeamRunGoalUpdate(args.base, args.teamId, args.runId, { event }, args.auth);
      if (!resp.ok) {
        setGoalUpdateError(resp.error || resp.err || "goal event failed");
        return;
      }
      setGoalUpdateNote(`goal ${eventType} event emitted`);
      await args.onRefreshRun(args.runId);
    } catch (err) {
      setGoalUpdateError(String(err));
    } finally {
      setGoalUpdateBusy(false);
    }
  };

  const emitHandoffEvent = async (
    mode: "manual" | "transition",
    nextState: TeamRunHandoffState = "proposed",
    seed?: TeamRunHandoffEventRecord,
  ) => {
    setHandoffError(null);
    setHandoffNote(null);
    if (!args.canWrite) {
      setHandoffError("missing team or run id");
      return;
    }
    const fromRole = (mode === "manual" ? handoffFromRole : seed?.from_role || "").trim();
    const toRole = (mode === "manual" ? handoffToRole : seed?.to_role || "").trim();
    const kindRaw = (mode === "manual" ? handoffKind : seed?.kind || "role").trim().toLowerCase();
    const handoffId = (mode === "manual" ? "" : seed?.handoff_id || "").trim();
    if (!handoffId && (!fromRole || !toRole)) {
      setHandoffError("handoff requires from_role and to_role");
      return;
    }
    if (kindRaw !== "role" && kindRaw !== "cross_deployment") {
      setHandoffError("handoff kind must be role or cross_deployment");
      return;
    }
    const kind = kindRaw as TeamRunHandoffKind;
    let dataObj: Record<string, unknown> | undefined;
    const rawData = mode === "manual" ? handoffData.trim() : "";
    if (rawData) {
      try {
        const parsed = JSON.parse(rawData) as unknown;
        if (!parsed || typeof parsed !== "object" || Array.isArray(parsed)) {
          setHandoffError("handoff data must be a JSON object");
          return;
        }
        dataObj = parsed as Record<string, unknown>;
      } catch (err) {
        setHandoffError(`handoff data invalid json: ${String(err)}`);
        return;
      }
    }
    const event: TeamRunHandoffEventInput = { kind, state: nextState };
    if (handoffId) event.handoff_id = handoffId;
    if (fromRole) event.from_role = fromRole;
    if (toRole) event.to_role = toRole;
    const reason = (mode === "manual" ? handoffReason : seed?.reason || "").trim();
    const message = (mode === "manual" ? handoffMessage : "").trim();
    if (reason) event.reason = reason;
    if (message) event.message = message;
    else if (mode === "transition") event.message = nextState;
    const sourceDeploymentId = (
      mode === "manual" ? handoffSourceDeployment : seed?.source_deployment_id || ""
    ).trim();
    const sourceSessionId = (mode === "manual" ? handoffSourceSession : seed?.source_session_id || "").trim();
    const targetDeploymentId = (
      mode === "manual" ? handoffTargetDeployment : seed?.target_deployment_id || ""
    ).trim();
    const targetSessionId = (mode === "manual" ? handoffTargetSession : seed?.target_session_id || "").trim();
    if (kind === "cross_deployment") {
      if (!sourceDeploymentId || !sourceSessionId || !targetDeploymentId || !targetSessionId) {
        setHandoffError("cross-deployment handoff requires source/target deployment and session ids");
        return;
      }
      event.source_deployment_id = sourceDeploymentId;
      event.source_session_id = sourceSessionId;
      event.target_deployment_id = targetDeploymentId;
      event.target_session_id = targetSessionId;
    }
    if (dataObj) event.data = dataObj;
    setHandoffBusy(true);
    try {
      const resp = await apiBrokerTeamRunHandoff(args.base, args.teamId, args.runId, { event }, args.auth);
      if (!resp.ok) {
        setHandoffError(resp.error || resp.err || "handoff event failed");
        return;
      }
      setHandoffNote(nextState === "proposed" ? "handoff event emitted" : `handoff ${nextState}`);
      await args.onRefreshRun(args.runId);
    } catch (err) {
      setHandoffError(String(err));
    } finally {
      setHandoffBusy(false);
    }
  };

  return {
    goalContract,
    goalEvents,
    goalEventRows: goalEvents.slice(-6).reverse(),
    goalContractGoal,
    setGoalContractGoal,
    goalContractCriteria,
    setGoalContractCriteria,
    goalContractConstraints,
    setGoalContractConstraints,
    goalEventType,
    setGoalEventType,
    goalEventMessage,
    setGoalEventMessage,
    goalEventData,
    setGoalEventData,
    goalUpdateBusy,
    goalUpdateError,
    goalUpdateNote,
    handleGoalContractUpdate,
    handleGoalEvent,
    handoffEventRows: args.handoffEvents.slice(-6).reverse(),
    handoffLatestById,
    handoffFromRole,
    setHandoffFromRole,
    handoffToRole,
    setHandoffToRole,
    handoffKind,
    setHandoffKind,
    handoffReason,
    setHandoffReason,
    handoffMessage,
    setHandoffMessage,
    handoffData,
    setHandoffData,
    handoffSourceDeployment,
    setHandoffSourceDeployment,
    handoffSourceSession,
    setHandoffSourceSession,
    handoffTargetDeployment,
    setHandoffTargetDeployment,
    handoffTargetSession,
    setHandoffTargetSession,
    handoffBusy,
    handoffError,
    handoffNote,
    handleHandoffEvent: async () => emitHandoffEvent("manual", "proposed"),
    handleHandoffTransition: async (
      seed: TeamRunHandoffEventRecord,
      nextState: TeamRunHandoffTransitionState,
    ) => emitHandoffEvent("transition", nextState, seed),
  };
}
