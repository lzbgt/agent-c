import React from "react";
import {
  apiBrokerTeamGuidanceAck,
  apiBrokerTeamGuidanceCreate,
  apiBrokerTeamGuidanceList,
  apiBrokerTeamGuidanceReceiptsList,
} from "../../api";
import { GUIDANCE_EVENT_TYPES, parseCsvList } from "./teamRunUtils";
import { normalizeGuidanceList } from "./brokerTeamGuidanceUtils";
import type { BrokerTeamGuidancePanelProps, BrokerTeamGuidanceState } from "./brokerTeamGuidanceTypes";

export default function useBrokerTeamGuidanceState(props: BrokerTeamGuidancePanelProps): BrokerTeamGuidanceState {
  const teamIdTrimmed = String(props.teamId || "").trim();
  const canQuery = props.canQuery && teamIdTrimmed.length > 0;

  const [guidance, setGuidance] = React.useState<any[]>([]);
  const [listBusy, setListBusy] = React.useState(false);
  const [listError, setListError] = React.useState<string | null>(null);
  const [statusFilter, setStatusFilter] = React.useState<string>("open");
  const [teamRunFilter, setTeamRunFilter] = React.useState<string>("");
  const [kind, setKind] = React.useState<string>("directive");
  const [priority, setPriority] = React.useState<string>("normal");
  const [message, setMessage] = React.useState<string>("");
  const [payloadJson, setPayloadJson] = React.useState<string>("");
  const [targetRoles, setTargetRoles] = React.useState<string>("");
  const [targetOrchestrator, setTargetOrchestrator] = React.useState<string>("");
  const [expiresUnixMs, setExpiresUnixMs] = React.useState<string>("");
  const [createBusy, setCreateBusy] = React.useState(false);
  const [createError, setCreateError] = React.useState<string | null>(null);
  const [ackNote, setAckNote] = React.useState<string>("");
  const [ackBusyId, setAckBusyId] = React.useState<string>("");
  const [ackError, setAckError] = React.useState<string | null>(null);
  const [receiptsByGuidanceId, setReceiptsByGuidanceId] = React.useState<Record<string, any[]>>({});
  const [receiptsBusyId, setReceiptsBusyId] = React.useState<string>("");
  const [receiptsErrorByGuidanceId, setReceiptsErrorByGuidanceId] = React.useState<Record<string, string>>({});
  const [receiptsOpenByGuidanceId, setReceiptsOpenByGuidanceId] = React.useState<Record<string, boolean>>({});
  const [briefingOpenByGuidanceId, setBriefingOpenByGuidanceId] = React.useState<Record<string, boolean>>({});
  const lastEventTsRef = React.useRef<number>(0);

  const guidanceEvents = React.useMemo(() => {
    const rows = Array.isArray(props.events) ? props.events : [];
    return rows.filter((row) => GUIDANCE_EVENT_TYPES.has(String(row?.type || "")));
  }, [props.events]);

  const loadGuidance = React.useCallback(async () => {
    if (!canQuery) return;
    setListBusy(true);
    setListError(null);
    try {
      const resp = await apiBrokerTeamGuidanceList(
        props.base,
        teamIdTrimmed,
        {
          status: statusFilter,
          teamRunId: teamRunFilter.trim() || undefined,
          limit: 200,
        },
        props.auth,
      );
      if (!resp.ok) throw new Error(resp.error || resp.err || "guidance list failed");
      setGuidance(normalizeGuidanceList(resp.guidance));
    } catch (err) {
      setListError(String(err));
      setGuidance([]);
    } finally {
      setListBusy(false);
    }
  }, [canQuery, props.base, props.auth, statusFilter, teamIdTrimmed, teamRunFilter]);

  React.useEffect(() => {
    void loadGuidance();
  }, [loadGuidance]);

  React.useEffect(() => {
    if (!canQuery || guidanceEvents.length === 0) return;
    const maxTs = guidanceEvents.reduce((acc, row) => Math.max(acc, row.ts_unix_ms || 0), 0);
    if (maxTs <= 0 || maxTs <= lastEventTsRef.current) return;
    lastEventTsRef.current = maxTs;
    void loadGuidance();
  }, [canQuery, guidanceEvents, loadGuidance]);

  const loadReceipts = React.useCallback(
    async (guidanceId: string) => {
      if (!canQuery || !guidanceId) return;
      setReceiptsBusyId(guidanceId);
      setReceiptsErrorByGuidanceId((prev) => ({ ...prev, [guidanceId]: "" }));
      try {
        const resp = await apiBrokerTeamGuidanceReceiptsList(
          props.base,
          teamIdTrimmed,
          guidanceId,
          { limit: 50 },
          props.auth,
        );
        if (!resp.ok) throw new Error(resp.error || resp.err || "receipts failed");
        setReceiptsByGuidanceId((prev) => ({ ...prev, [guidanceId]: Array.isArray(resp.receipts) ? resp.receipts : [] }));
      } catch (err) {
        setReceiptsErrorByGuidanceId((prev) => ({ ...prev, [guidanceId]: String(err) }));
        setReceiptsByGuidanceId((prev) => ({ ...prev, [guidanceId]: [] }));
      } finally {
        setReceiptsBusyId("");
      }
    },
    [canQuery, props.auth, props.base, teamIdTrimmed],
  );

  const handleCreate = React.useCallback(async () => {
    if (!canQuery) return;
    const trimmedMessage = message.trim();
    if (!trimmedMessage) {
      setCreateError("message required");
      return;
    }
    setCreateBusy(true);
    setCreateError(null);
    try {
      let payload: Record<string, any> | undefined;
      const rawPayload = payloadJson.trim();
      if (rawPayload) payload = JSON.parse(rawPayload);
      const body: Record<string, any> = { kind, priority, message: trimmedMessage };
      const teamRun = teamRunFilter.trim();
      if (teamRun) body.team_run_id = teamRun;
      if (payload) body.payload = payload;
      const roles = parseCsvList(targetRoles);
      if (roles.length > 0) body.target_roles = roles;
      const orchestrator = targetOrchestrator.trim();
      if (orchestrator) body.target_orchestrator_id = orchestrator;
      const expires = expiresUnixMs.trim();
      if (expires) body.expires_unix_ms = Number(expires);
      const resp = await apiBrokerTeamGuidanceCreate(props.base, teamIdTrimmed, body, props.auth);
      if (!resp.ok) throw new Error(resp.error || resp.err || "create guidance failed");
      setMessage("");
      setPayloadJson("");
      setExpiresUnixMs("");
      await loadGuidance();
    } catch (err) {
      setCreateError(String(err));
    } finally {
      setCreateBusy(false);
    }
  }, [
    canQuery,
    expiresUnixMs,
    kind,
    loadGuidance,
    message,
    payloadJson,
    priority,
    props.auth,
    props.base,
    targetOrchestrator,
    targetRoles,
    teamIdTrimmed,
    teamRunFilter,
  ]);

  const handleAck = React.useCallback(
    async (guidanceId: string) => {
      if (!canQuery || !guidanceId) return;
      setAckBusyId(guidanceId);
      setAckError(null);
      try {
        const resp = await apiBrokerTeamGuidanceAck(
          props.base,
          teamIdTrimmed,
          guidanceId,
          { status: "acked", note: ackNote.trim(), ack_source: "human" },
          props.auth,
        );
        if (!resp.ok) throw new Error(resp.error || resp.err || "ack failed");
        await loadGuidance();
        if (receiptsOpenByGuidanceId[guidanceId]) {
          await loadReceipts(guidanceId);
        }
      } catch (err) {
        setAckError(String(err));
      } finally {
        setAckBusyId("");
      }
    },
    [ackNote, canQuery, loadGuidance, loadReceipts, props.auth, props.base, receiptsOpenByGuidanceId, teamIdTrimmed],
  );

  const toggleReceipts = React.useCallback(
    (guidanceId: string) => {
      if (!guidanceId) return;
      const nextOpen = !receiptsOpenByGuidanceId[guidanceId];
      setReceiptsOpenByGuidanceId((prev) => ({ ...prev, [guidanceId]: nextOpen }));
      if (nextOpen && !Object.prototype.hasOwnProperty.call(receiptsByGuidanceId, guidanceId)) {
        void loadReceipts(guidanceId);
      }
    },
    [loadReceipts, receiptsByGuidanceId, receiptsOpenByGuidanceId],
  );

  const toggleBriefing = React.useCallback((guidanceId: string) => {
    if (!guidanceId) return;
    setBriefingOpenByGuidanceId((prev) => ({ ...prev, [guidanceId]: !prev[guidanceId] }));
  }, []);

  return {
    canQuery,
    teamIdTrimmed,
    guidanceEvents,
    guidanceRows: normalizeGuidanceList(guidance),
    listBusy,
    listError,
    statusFilter,
    teamRunFilter,
    kind,
    priority,
    message,
    payloadJson,
    targetRoles,
    targetOrchestrator,
    expiresUnixMs,
    createBusy,
    createError,
    ackNote,
    ackBusyId,
    ackError,
    receiptsByGuidanceId,
    receiptsBusyId,
    receiptsErrorByGuidanceId,
    receiptsOpenByGuidanceId,
    briefingOpenByGuidanceId,
    setStatusFilter,
    setTeamRunFilter,
    setKind,
    setPriority,
    setMessage,
    setPayloadJson,
    setTargetRoles,
    setTargetOrchestrator,
    setExpiresUnixMs,
    setAckNote,
    loadGuidance,
    handleCreate,
    handleAck,
    loadReceipts,
    toggleReceipts,
    toggleBriefing,
  };
}
