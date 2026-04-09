import React from "react";

import {
  apiBrokerTeamRunModeratorDirective,
  apiBrokerTeamRunModeratorEvents,
  apiBrokerTeamRunModeratorTask,
  type ApiAuth,
} from "../../api";
import type { TeamRunModeratorEventRow } from "./teamRunStatusTypes";
import { parseCsvList } from "./teamRunUtils";

type UseBrokerTeamRunModeratorStateArgs = {
  base: string;
  auth: ApiAuth;
  teamIdTrimmed: string;
  resolveRunId: () => string;
};

type ModeratorTargets = {
  roles?: string[];
  member_ids?: string[];
  agent_ids?: string[];
};

type ModeratorDirectivePayload = {
  directive: string;
  scope?: string;
  assignees?: string[];
  targets?: ModeratorTargets;
  append_to_session: boolean;
};

type ModeratorTaskPayload = {
  title: string;
  detail?: string;
  status?: string;
  assignees?: string[];
  targets?: ModeratorTargets;
  append_to_session: boolean;
};

export default function useBrokerTeamRunModeratorState({
  base,
  auth,
  teamIdTrimmed,
  resolveRunId,
}: UseBrokerTeamRunModeratorStateArgs) {
  const [moderatorDirective, setModeratorDirective] = React.useState<string>("");
  const [moderatorDirectiveScope, setModeratorDirectiveScope] = React.useState<string>("");
  const [moderatorTaskTitle, setModeratorTaskTitle] = React.useState<string>("");
  const [moderatorTaskDetail, setModeratorTaskDetail] = React.useState<string>("");
  const [moderatorTaskStatus, setModeratorTaskStatus] = React.useState<string>("");
  const [moderatorTargetRoles, setModeratorTargetRoles] = React.useState<string>("");
  const [moderatorTargetMembers, setModeratorTargetMembers] = React.useState<string>("");
  const [moderatorTargetAgents, setModeratorTargetAgents] = React.useState<string>("");
  const [moderatorAssignees, setModeratorAssignees] = React.useState<string>("");
  const [moderatorAppendToSession, setModeratorAppendToSession] = React.useState<boolean>(false);
  const [moderatorBusy, setModeratorBusy] = React.useState<boolean>(false);
  const [moderatorError, setModeratorError] = React.useState<string | null>(null);
  const [moderatorSuccess, setModeratorSuccess] = React.useState<string | null>(null);
  const [moderatorEvents, setModeratorEvents] = React.useState<TeamRunModeratorEventRow[]>([]);
  const [moderatorEventsBusy, setModeratorEventsBusy] = React.useState<boolean>(false);
  const [moderatorEventsError, setModeratorEventsError] = React.useState<string | null>(null);
  const [moderatorEventsTypes, setModeratorEventsTypes] = React.useState<string>(
    "moderator_directive,moderator_task_published",
  );
  const [moderatorEventsMaxBytes, setModeratorEventsMaxBytes] = React.useState<string>("1048576");
  const [moderatorEventsLimit, setModeratorEventsLimit] = React.useState<string>("200");
  const [moderatorEventsExpanded, setModeratorEventsExpanded] = React.useState<boolean>(false);

  const buildModeratorTargets = React.useCallback(() => {
    const roles = parseCsvList(moderatorTargetRoles).map((role) => role.toLowerCase());
    const members = parseCsvList(moderatorTargetMembers);
    const agents = parseCsvList(moderatorTargetAgents);
    if (roles.length === 0 && members.length === 0 && agents.length === 0) return undefined;
    return { roles, member_ids: members, agent_ids: agents } satisfies ModeratorTargets;
  }, [moderatorTargetAgents, moderatorTargetMembers, moderatorTargetRoles]);

  const handleModeratorDirectivePublish = React.useCallback(async () => {
    const runId = resolveRunId();
    if (!teamIdTrimmed || !runId) {
      setModeratorError("missing team_id or run id");
      return;
    }
    const directive = String(moderatorDirective || "").trim();
    if (!directive) {
      setModeratorError("directive required");
      return;
    }
    setModeratorBusy(true);
    setModeratorError(null);
    setModeratorSuccess(null);
    try {
      const assignees = parseCsvList(moderatorAssignees);
      const payload: ModeratorDirectivePayload = {
        directive,
        scope: String(moderatorDirectiveScope || "").trim() || undefined,
        assignees: assignees.length > 0 ? assignees : undefined,
        targets: buildModeratorTargets(),
        append_to_session: moderatorAppendToSession,
      };
      if (!payload.scope) delete payload.scope;
      if (!payload.targets) delete payload.targets;
      const resp = await apiBrokerTeamRunModeratorDirective(base, teamIdTrimmed, runId, payload, auth);
      if (!resp.ok) {
        throw new Error(resp.error || resp.err || resp.code || "moderator directive failed");
      }
      const dispatched = Array.isArray(resp.dispatched) ? resp.dispatched.length : 0;
      const skipped = Array.isArray(resp.skipped) ? resp.skipped.length : 0;
      setModeratorSuccess(`directive dispatched to ${dispatched}${skipped ? ` (skipped ${skipped})` : ""}`);
      setModeratorDirective("");
      setModeratorDirectiveScope("");
    } catch (err) {
      setModeratorError(String(err));
    } finally {
      setModeratorBusy(false);
    }
  }, [
    auth,
    base,
    buildModeratorTargets,
    moderatorAppendToSession,
    moderatorAssignees,
    moderatorDirective,
    moderatorDirectiveScope,
    resolveRunId,
    teamIdTrimmed,
  ]);

  const handleModeratorTaskPublish = React.useCallback(async () => {
    const runId = resolveRunId();
    if (!teamIdTrimmed || !runId) {
      setModeratorError("missing team_id or run id");
      return;
    }
    const title = String(moderatorTaskTitle || "").trim();
    if (!title) {
      setModeratorError("task title required");
      return;
    }
    setModeratorBusy(true);
    setModeratorError(null);
    setModeratorSuccess(null);
    try {
      const assignees = parseCsvList(moderatorAssignees);
      const payload: ModeratorTaskPayload = {
        title,
        detail: String(moderatorTaskDetail || "").trim() || undefined,
        status: String(moderatorTaskStatus || "").trim() || undefined,
        assignees: assignees.length > 0 ? assignees : undefined,
        targets: buildModeratorTargets(),
        append_to_session: moderatorAppendToSession,
      };
      if (!payload.detail) delete payload.detail;
      if (!payload.status) delete payload.status;
      if (!payload.targets) delete payload.targets;
      const resp = await apiBrokerTeamRunModeratorTask(base, teamIdTrimmed, runId, payload, auth);
      if (!resp.ok) {
        throw new Error(resp.error || resp.err || resp.code || "moderator task failed");
      }
      const dispatched = Array.isArray(resp.dispatched) ? resp.dispatched.length : 0;
      const skipped = Array.isArray(resp.skipped) ? resp.skipped.length : 0;
      setModeratorSuccess(`task dispatched to ${dispatched}${skipped ? ` (skipped ${skipped})` : ""}`);
      setModeratorTaskTitle("");
      setModeratorTaskDetail("");
      setModeratorTaskStatus("");
    } catch (err) {
      setModeratorError(String(err));
    } finally {
      setModeratorBusy(false);
    }
  }, [
    auth,
    base,
    buildModeratorTargets,
    moderatorAppendToSession,
    moderatorAssignees,
    moderatorTaskDetail,
    moderatorTaskStatus,
    moderatorTaskTitle,
    resolveRunId,
    teamIdTrimmed,
  ]);

  const handleModeratorEventsLoad = React.useCallback(async () => {
    const runId = resolveRunId();
    if (!teamIdTrimmed || !runId) {
      setModeratorEventsError("missing team_id or run id");
      return;
    }
    setModeratorEventsBusy(true);
    setModeratorEventsError(null);
    try {
      const maxBytesRaw = Number.parseInt(String(moderatorEventsMaxBytes || ""), 10);
      const limitRaw = Number.parseInt(String(moderatorEventsLimit || ""), 10);
      const resp = await apiBrokerTeamRunModeratorEvents(
        base,
        teamIdTrimmed,
        runId,
        {
          types: String(moderatorEventsTypes || "").trim(),
          maxBytes: Number.isFinite(maxBytesRaw) ? maxBytesRaw : undefined,
          limit: Number.isFinite(limitRaw) ? limitRaw : undefined,
          roles: String(moderatorTargetRoles || "").trim(),
          memberIds: String(moderatorTargetMembers || "").trim(),
          agentIds: String(moderatorTargetAgents || "").trim(),
        },
        auth,
      );
      if (!resp.ok) {
        throw new Error(resp.error || resp.err || resp.code || "moderator events failed");
      }
      setModeratorEvents(resp.events ?? []);
      if (resp.skipped && Array.isArray(resp.skipped) && resp.skipped.length > 0) {
        setModeratorSuccess(`loaded ${resp.events?.length ?? 0} events (skipped ${resp.skipped.length})`);
      } else {
        setModeratorSuccess(`loaded ${resp.events?.length ?? 0} events`);
      }
    } catch (err) {
      setModeratorEventsError(String(err));
    } finally {
      setModeratorEventsBusy(false);
    }
  }, [
    auth,
    base,
    moderatorEventsLimit,
    moderatorEventsMaxBytes,
    moderatorEventsTypes,
    moderatorTargetAgents,
    moderatorTargetMembers,
    moderatorTargetRoles,
    resolveRunId,
    teamIdTrimmed,
  ]);

  return {
    moderatorDirective,
    setModeratorDirective,
    moderatorDirectiveScope,
    setModeratorDirectiveScope,
    moderatorTaskTitle,
    setModeratorTaskTitle,
    moderatorTaskDetail,
    setModeratorTaskDetail,
    moderatorTaskStatus,
    setModeratorTaskStatus,
    moderatorTargetRoles,
    setModeratorTargetRoles,
    moderatorTargetMembers,
    setModeratorTargetMembers,
    moderatorTargetAgents,
    setModeratorTargetAgents,
    moderatorAssignees,
    setModeratorAssignees,
    moderatorAppendToSession,
    setModeratorAppendToSession,
    moderatorBusy,
    moderatorError,
    moderatorSuccess,
    moderatorEvents,
    moderatorEventsBusy,
    moderatorEventsError,
    moderatorEventsTypes,
    setModeratorEventsTypes,
    moderatorEventsMaxBytes,
    setModeratorEventsMaxBytes,
    moderatorEventsLimit,
    setModeratorEventsLimit,
    moderatorEventsExpanded,
    setModeratorEventsExpanded,
    handleModeratorDirectivePublish,
    handleModeratorTaskPublish,
    handleModeratorEventsLoad,
  };
}
