import type { AgentEvent } from "../../api";
import type { SceneEntityMutationOp } from "../scene/sceneViewTypes";
import type { ConversationUiActionRpcRequest } from "./conversationRpcTypes";
import type { ConversationToolCallSummaryById } from "./conversationViewTypes";
import { normalizeEventData, safeJsonParse, safeObject, safeTrunc, type UnknownRecord } from "./utils";

const RPC_SIDE_EFFECT_KINDS = new Set([
  "dom_click",
  "dom_set_value",
  "dom_apply",
  "entity_apply",
  "media_play",
  "media_pause",
  "media_observe",
  "navigate",
  "open_url",
  "page_eval",
]);

export type ConversationToolCallPreview = {
  label: "cmd" | "argv";
  value: string;
};

export type ParsedConversationUiAction = {
  raw: UnknownRecord;
  action: UnknownRecord;
  atype: string;
  title: string;
  toolCallId: string;
  queryId: string;
  rpc: UnknownRecord;
  rpcId: string;
  rpcKind: string;
  rpcArgs: UnknownRecord;
  sideEffectsRequested: boolean;
  autoRunRequested: boolean;
};

const asTrimmedString = (value: unknown): string => (typeof value === "string" ? value.trim() : "");

const asStringArray = (value: unknown): string[] =>
  Array.isArray(value) ? value.map((item) => (typeof item === "string" ? item : "")).filter(Boolean) : [];

export const getNormalizedEventData = (data: unknown): unknown => normalizeEventData(data);

export const getNormalizedEventRecord = (data: unknown): UnknownRecord => safeObject(getNormalizedEventData(data));

export const parseConversationUiAction = (value: unknown): ParsedConversationUiAction => {
  const raw = safeObject(value);
  const action = safeObject(raw.action);
  const atype = asTrimmedString(action.type);
  const title = asTrimmedString(action.title) || (atype ? `ui_action: ${atype}` : "ui_action");
  const toolCallId = asTrimmedString(raw.tool_call_id);
  const queryId = asTrimmedString(action.query_id) || toolCallId;
  const rpc = safeObject(action.rpc ?? action.probe);
  const rpcId = asTrimmedString(action.rpc_id) || asTrimmedString(action.probe_id) || toolCallId;
  const rpcKind = asTrimmedString(rpc.kind);
  const rpcArgs = safeObject(rpc.args ?? rpc);
  const sideEffectsRequested = rpc.side_effects === true || action.side_effects === true || RPC_SIDE_EFFECT_KINDS.has(rpcKind);
  const autoRunRequested =
    typeof action.auto_run === "boolean" ? action.auto_run : typeof action.auto === "boolean" ? action.auto : true;
  return {
    raw,
    action,
    atype,
    title,
    toolCallId,
    queryId,
    rpc,
    rpcId,
    rpcKind,
    rpcArgs,
    sideEffectsRequested,
    autoRunRequested,
  };
};

export const deriveConversationRpcRequest = (
  value: unknown,
  sessionId?: string,
  allowClientRpcs: boolean = false,
  allowClientEffects: boolean = false,
): ConversationUiActionRpcRequest => {
  const parsed = parseConversationUiAction(value);
  const canRun = !!parsed.rpcId && typeof sessionId === "string" && sessionId.trim().length > 0;
  const canRunAuto = !!allowClientRpcs && (!parsed.sideEffectsRequested || !!allowClientEffects);
  return {
    atype: parsed.atype,
    title: parsed.title,
    toolCallId: parsed.toolCallId,
    rpcId: parsed.rpcId,
    rpcKind: parsed.rpcKind,
    rpcArgs: parsed.rpcArgs,
    sideEffectsRequested: parsed.sideEffectsRequested,
    autoRunRequested: parsed.autoRunRequested,
    canRun,
    canRunAuto,
    autoRun: canRunAuto && parsed.autoRunRequested,
  };
};

export const buildEntityApplyOps = (value: unknown): SceneEntityMutationOp[] => {
  const args = safeObject(value);
  if (Array.isArray(args.ops)) {
    return args.ops.flatMap((item): SceneEntityMutationOp[] => {
      const op = safeObject(item);
      const kind = asTrimmedString(op.op) || asTrimmedString(op.kind);
      if (kind === "create") {
        const entityKind = asTrimmedString(op.entity_kind) || asTrimmedString(op.entityKind);
        if (!entityKind) return [];
        return [
          {
            op: "create" as const,
            id: asTrimmedString(op.id) || undefined,
            entity_kind: entityKind,
            title: asTrimmedString(op.title) || undefined,
            props: safeObject(op.props),
          },
        ];
      }
      if (kind === "update") {
        const id = asTrimmedString(op.id);
        if (!id) return [];
        return [{ op: "update" as const, id, props: safeObject(op.props) }];
      }
      if (kind === "delete" || kind === "remove") {
        const id = asTrimmedString(op.id);
        if (!id) return [];
        return [{ op: kind as "delete" | "remove", id }];
      }
      if (kind === "action") {
        const id = asTrimmedString(op.id);
        const action = asTrimmedString(op.action);
        if (!id || !action) return [];
        return [{ op: "action" as const, id, action, args: safeObject(op.args) }];
      }
      if (kind === "clear") {
        return [{ op: "clear" as const, color: asTrimmedString(op.color) || undefined }];
      }
      return [];
    });
  }

  if (Array.isArray(args.entities)) {
    const ops: SceneEntityMutationOp[] = [];
    for (const entityValue of args.entities.slice(0, 50)) {
      const entity = safeObject(entityValue);
      const id = safeTrunc(asTrimmedString(entity.id), 200);
      const entityKind = safeTrunc(
        asTrimmedString(entity.entity_kind) || asTrimmedString(entity.entityKind) || asTrimmedString(entity.type) || asTrimmedString(entity.kind),
        100,
      );
      if (!id || !entityKind) continue;
      const title = asTrimmedString(entity.title);
      const props = safeObject(entity.props ?? entity);
      ops.push({ op: "create", id, entity_kind: entityKind, title: title || undefined, props });
      const actions = Array.isArray(entity.actions) ? entity.actions : [];
      for (const actionValue of actions.slice(0, 20)) {
        const action = safeObject(actionValue);
        const name = safeTrunc(
          asTrimmedString(action.name) || asTrimmedString(action.action) || asTrimmedString(action.kind),
          80,
        );
        if (!name) continue;
        ops.push({ op: "action", id, action: name, args: safeObject(action.args ?? action) });
      }
    }
    return ops;
  }

  const id = safeTrunc(asTrimmedString(args.id), 200);
  const entityKind = safeTrunc(
    asTrimmedString(args.entity_kind) || asTrimmedString(args.entityKind) || asTrimmedString(args.type) || asTrimmedString(args.kind),
    100,
  );
  const props = safeObject(args.props);
  const titleFromProps = asTrimmedString(props.title);
  const titleFromArgs = safeTrunc(asTrimmedString(args.title), 200);
  const title = titleFromArgs || titleFromProps || "";
  const ops: SceneEntityMutationOp[] = [];

  if (id && entityKind && (Object.keys(props).length > 0 || title)) {
    ops.push({ op: "create", id, entity_kind: entityKind, title: title || undefined, props });
  } else if (id && Object.keys(props).length > 0) {
    ops.push({ op: "update", id, props });
  }

  const actions = Array.isArray(args.actions) ? args.actions : [];
  for (const actionValue of actions.slice(0, 20)) {
    const action = safeObject(actionValue);
    const name = safeTrunc(asTrimmedString(action.name) || asTrimmedString(action.action), 80);
    if (!name) continue;
    ops.push({ op: "action", id, action: name, args: safeObject(action.args) });
  }

  const singleAction = safeTrunc(asTrimmedString(args.action), 80);
  if (singleAction && id) {
    ops.push({ op: "action", id, action: singleAction, args: safeObject(args.args) });
  }
  if (id && (args.delete === true || args.remove === true)) {
    ops.push({ op: "delete", id });
  }
  if (args.clear === true) {
    ops.push({ op: "clear" });
  }
  return ops;
};

export const capEventPayload = (value: unknown): unknown => {
  try {
    const serialized = JSON.stringify(value);
    const max = 32 * 1024;
    if (serialized.length <= max) return value;
    return { kind: "truncated", bytes: serialized.length, preview: serialized.slice(0, 2000) };
  } catch {
    return { kind: "unserializable" };
  }
};

export const extractToolCallArgumentsText = (data: UnknownRecord): string => {
  if (typeof data.arguments_json === "string") return data.arguments_json;
  if (Object.keys(safeObject(data.arguments)).length > 0) {
    try {
      return JSON.stringify(data.arguments);
    } catch {
      return "";
    }
  }
  if (Object.keys(safeObject(data.args)).length > 0) {
    try {
      return JSON.stringify(data.args);
    } catch {
      return "";
    }
  }
  return "";
};

export const buildToolCallPreviewFromArgs = (toolName: string, argsText: string): ConversationToolCallPreview | null => {
  const parsed = argsText ? safeJsonParse(argsText) : null;
  const record = safeObject(parsed);
  if (toolName === "shell_exec") {
    const cmd = asTrimmedString(record.cmd);
    return cmd ? { label: "cmd", value: cmd } : null;
  }
  if (toolName === "proc_exec") {
    const argv = asStringArray(record.argv).join(" ");
    return argv ? { label: "argv", value: argv } : null;
  }
  return null;
};

export const buildConversationToolCallSummaryById = (events: AgentEvent[]): ConversationToolCallSummaryById => {
  const summaries: ConversationToolCallSummaryById = {};
  for (const event of events) {
    if (event.type !== "tool_result") continue;
    const data = getNormalizedEventRecord(event.data);
    const toolCallId = asTrimmedString(data.tool_call_id);
    if (!toolCallId) continue;
    const summary = safeObject(data.summary);
    if (Object.keys(summary).length === 0) continue;
    const cmd = asTrimmedString(summary.cmd);
    const argv = asStringArray(summary.argv).join(" ");
    if (cmd || argv) {
      summaries[toolCallId] = { cmd: cmd || undefined, argv: argv || undefined };
    }
  }
  return summaries;
};
