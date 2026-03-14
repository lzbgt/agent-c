import React from "react";
import {
  apiPostSessionSceneApply,
  apiPostSessionUiEvent,
  daemonFetchInit,
  type ApiAuth,
} from "../api";
import type { SceneEntity } from "../components/SceneView";
import { SCENE_STORE_MAX, touchSceneStoreKey } from "../sceneCache";

type UseRuntimeClientEffectsArgs = {
  allowClientEffects: boolean;
  allowClientRpcs: boolean;
  artifactCatalogSupported: boolean;
  client: { id: string; kind: string; instance_id: string };
  daemonAuth: ApiAuth;
  dbClientEventsData: any;
  dbUiActionsData: any;
  effectiveBase: string;
  sessionArtifactsData: any;
  sessionId: string;
  sessionSceneData: any;
  sessionScopeKey: string;
  yolo: boolean;
};

export function useRuntimeClientEffects(args: UseRuntimeClientEffectsArgs) {
  const {
    allowClientEffects,
    allowClientRpcs,
    artifactCatalogSupported,
    client,
    daemonAuth,
    dbClientEventsData,
    dbUiActionsData,
    effectiveBase,
    sessionArtifactsData,
    sessionId,
    sessionSceneData,
    sessionScopeKey,
    yolo,
  } = args;

  const sceneBySessionRef = React.useRef<Record<string, Record<string, SceneEntity>>>({});
  const [sceneVersion, setSceneVersion] = React.useState<number>(0);
  const sceneStoreOrderRef = React.useRef<string[]>([]);
  const lastSceneUpdatedMsRef = React.useRef<Record<string, number>>({});
  const sceneStoreKey = `${sessionScopeKey}::${sessionId}`;

  const sceneEntities = React.useMemo(() => {
    const map = sceneBySessionRef.current[sceneStoreKey] || {};
    return Object.values(map);
  }, [sceneStoreKey, sceneVersion]);

  const touchSceneStore = React.useCallback((key: string) => {
    touchSceneStoreKey(sceneBySessionRef.current, sceneStoreOrderRef.current, lastSceneUpdatedMsRef.current, key, SCENE_STORE_MAX);
  }, []);

  const applySceneOps = React.useCallback(
    (sid: string, ops: any[]) => {
      const trimmedSessionId = String(sid || "").trim();
      if (!trimmedSessionId) throw new Error("missing session_id for scene ops");
      const storeKey = `${sessionScopeKey}::${trimmedSessionId}`;
      if (!sceneBySessionRef.current[storeKey]) sceneBySessionRef.current[storeKey] = {};
      const store = sceneBySessionRef.current[storeKey];
      touchSceneStore(storeKey);

      const now = Date.now();
      const results: any[] = [];
      const genId = () => `ent-${now}-${Math.random().toString(16).slice(2)}`;
      const getOpKind = (op: any): string => {
        const kind = typeof op?.op === "string" ? op.op : typeof op?.kind === "string" ? op.kind : "";
        return String(kind || "").trim();
      };
      const getCreateKind = (op: any): string => {
        const kind =
          typeof op?.entity_kind === "string" ? op.entity_kind : typeof op?.entityKind === "string" ? op.entityKind : "";
        return String(kind || "").trim();
      };
      const persistOps = (Array.isArray(ops) ? ops : []).filter((op) => getOpKind(op) !== "clear");

      for (const opRaw of (Array.isArray(ops) ? ops : []).slice(0, 100)) {
        try {
          const op = opRaw && typeof opRaw === "object" ? opRaw : {};
          const kind = getOpKind(op);
          if (!kind) throw new Error("missing op");
          if (kind === "create") {
            const id = String(op.id ?? "").trim() || genId();
            const entityKind = getCreateKind(op);
            if (!entityKind) throw new Error("create requires entity_kind");
            store[id] = {
              id,
              kind: entityKind,
              title: typeof op.title === "string" ? op.title : undefined,
              props: op.props ?? {},
              created_ms: now,
              updated_ms: now,
            };
            results.push({ ok: true, op: "create", id });
            continue;
          }
          if (kind === "update") {
            const id = String(op.id ?? "").trim();
            if (!id) throw new Error("update requires id");
            const existing = store[id];
            if (!existing) throw new Error("entity not found");
            existing.props = { ...(existing.props ?? {}), ...(op.props ?? {}) };
            existing.updated_ms = now;
            results.push({ ok: true, op: "update", id });
            continue;
          }
          if (kind === "delete" || kind === "remove") {
            const id = String(op.id ?? "").trim();
            if (!id) throw new Error("delete requires id");
            const existed = !!store[id];
            delete store[id];
            results.push({ ok: true, op: "delete", id, existed });
            continue;
          }
          if (kind === "clear") {
            results.push({ ok: false, op: "clear", error: "scene clear is disabled in WebUI" });
            continue;
          }
          if (kind === "action") {
            const id = String(op.id ?? "").trim();
            const action = String(op.action ?? "").trim();
            if (!id) throw new Error("action requires id");
            if (!action) throw new Error("action requires action");
            const existing = store[id];
            if (!existing) throw new Error("entity not found");
            existing.props = {
              ...(existing.props ?? {}),
              last_action: { name: action, args: op.args ?? {}, ts_unix_ms: now },
            };
            existing.updated_ms = now;
            results.push({ ok: true, op: "action", id, action });
            continue;
          }
          throw new Error(`unsupported op: ${kind}`);
        } catch (error) {
          results.push({ ok: false, error: String(error) });
        }
      }

      setSceneVersion((value) => value + 1);
      if (persistOps.length > 0) {
        void apiPostSessionSceneApply(effectiveBase, { session_id: trimmedSessionId, ops: persistOps }, daemonAuth)
          .then((resp) => {
            if (!resp || resp.ok !== true) return;
            const updated = typeof resp.updated_unix_ms === "number" ? resp.updated_unix_ms : 0;
            if (updated > 0) {
              lastSceneUpdatedMsRef.current[storeKey] = Math.max(lastSceneUpdatedMsRef.current[storeKey] || 0, updated);
            }
            const scene = resp.scene && typeof resp.scene === "object" && !Array.isArray(resp.scene) ? (resp.scene as any) : null;
            if (scene) {
              sceneBySessionRef.current[storeKey] = scene;
              touchSceneStore(storeKey);
              setSceneVersion((value) => value + 1);
            }
          })
          .catch(() => {});
      }
      return { ok: true, results, count: Object.keys(store).length };
    },
    [daemonAuth, effectiveBase, sessionScopeKey, touchSceneStore],
  );

  React.useEffect(() => {
    const sid = typeof sessionId === "string" ? sessionId.trim() : "";
    if (!sid) return;
    if (!sessionSceneData || sessionSceneData.ok !== true) return;
    const updated = typeof sessionSceneData?.updated_unix_ms === "number" ? sessionSceneData.updated_unix_ms : 0;
    const key = sceneStoreKey;
    const hasPrev = Object.prototype.hasOwnProperty.call(lastSceneUpdatedMsRef.current, key);
    const prev = hasPrev ? lastSceneUpdatedMsRef.current[key] || 0 : -1;
    if (hasPrev && updated <= prev) return;
    const scene = sessionSceneData?.scene;
    if (!scene || typeof scene !== "object" || Array.isArray(scene)) return;
    sceneBySessionRef.current[key] = scene as any;
    lastSceneUpdatedMsRef.current[key] = updated;
    touchSceneStore(key);
    setSceneVersion((value) => value + 1);
  }, [sceneStoreKey, sessionId, sessionSceneData, touchSceneStore]);

  const postedCapsRef = React.useRef<Record<string, boolean>>({});
  React.useEffect(() => {
    const sid = typeof sessionId === "string" ? sessionId.trim() : "";
    if (!sid) return;
    const key = `${effectiveBase}::${sid}`;
    if (postedCapsRef.current[key]) return;
    postedCapsRef.current[key] = true;
    void apiPostSessionUiEvent(
      effectiveBase,
      {
        session_id: sid,
        type: "client_capabilities",
        client,
        data: {
          rpcs: [
            { kind: "dom_query", side_effects: false, description: "Read-only DOM query (selector + bounded fields)." },
            { kind: "media_snapshot", side_effects: false, description: "Snapshot audio/video elements (paused/currentTime/duration)." },
            { kind: "location", side_effects: false, description: "Browser location (href/origin/path/search; query redacted)." },
            { kind: "state_snapshot", side_effects: false, description: "Combined snapshot (location + media_snapshot)." },
            { kind: "entity_query", side_effects: false, description: "Query client-side entities (scene objects)." },
            { kind: "entity_apply", side_effects: true, description: "Create/update/delete/action client-side entities (scene objects)." },
            { kind: "dom_apply", side_effects: true, description: "Apply a DOM patch (create/edit/delete/dispatch) by selector." },
            { kind: "dom_click", side_effects: true, description: "Click a DOM element by selector (side effects)." },
            { kind: "dom_set_value", side_effects: true, description: "Set input/textarea value by selector (side effects)." },
            { kind: "media_play", side_effects: true, description: "Attempt to play audio/video by selector (browser policies apply)." },
            { kind: "media_observe", side_effects: true, description: "Attach media listeners and emit correlated progress events." },
            { kind: "media_unobserve", side_effects: false, description: "Detach media listeners created by media_observe (by rpc_id or all=true)." },
            { kind: "navigate", side_effects: true, description: "Navigate the browser to a new URL (likely reloads the app)." },
            { kind: "open_url", side_effects: true, description: "Open an external URL in a new tab after explicit user confirmation." },
            {
              kind: "artifact_url",
              side_effects: false,
              description:
                "Resolve a daemon-served artifact path (out/...) to a browser-usable URL. Returns blob: URL when daemon auth is enabled.",
            },
            { kind: "script_eval", side_effects: false, description: "Run agent-provided script code in a killable worker with a DOM/media/location API bridge." },
            { kind: "page_eval", side_effects: true, description: "UNSAFE: run agent-provided JS on the main thread with access to DOM via an API bridge (cooperative async only)." },
          ],
          probes: [
            { kind: "dom_query", description: "Read-only DOM query (selector + bounded fields)." },
            { kind: "media_snapshot", description: "Snapshot audio/video elements." },
            { kind: "location", description: "Browser location." },
          ],
        },
        append_to_session: false,
      },
      daemonAuth,
    ).catch(() => {});
  }, [client, daemonAuth, effectiveBase, sessionId]);

  const appliedUiActionIdsRef = React.useRef<Record<string, number>>({});
  const appliedUiActionLimit = 2000;
  React.useEffect(() => {
    if (!allowClientRpcs || !allowClientEffects) return;
    const sid = String(sessionId || "").trim();
    if (!sid) return;
    const actionsRaw =
      dbUiActionsData?.ok && Array.isArray(dbUiActionsData?.ui_actions) ? (dbUiActionsData.ui_actions as any[]) : [];
    const clientEventsRaw =
      dbClientEventsData?.ok && Array.isArray(dbClientEventsData?.client_events) ? (dbClientEventsData.client_events as any[]) : [];

    const ackedRpcIds = new Set<string>();
    for (const clientEvent of clientEventsRaw) {
      const type = typeof clientEvent?.type === "string" ? clientEvent.type : "";
      if (type !== "client_rpc_result") continue;
      const data = clientEvent?.data ?? clientEvent?.data_json ?? {};
      const rpcId = typeof data?.rpc_id === "string" ? data.rpc_id : typeof data?.probe_id === "string" ? data.probe_id : "";
      if (rpcId) ackedRpcIds.add(rpcId);
    }

    const actions = actionsRaw.slice().reverse().filter((row) => row && typeof row === "object");
    const safeObject = (value: any) => (value && typeof value === "object" && !Array.isArray(value) ? value : {});
    const entityApplyArgsToOps = (args: any): any[] => {
      if (Array.isArray(args?.ops)) return args.ops.slice(0, 100);
      if (Array.isArray(args?.operations)) return args.operations.slice(0, 100);
      if (Array.isArray(args?.entities)) {
        const ops: any[] = [];
        for (const entity of (args.entities as any[]).slice(0, 50)) {
          if (!entity || typeof entity !== "object") continue;
          const id = String(entity?.id ?? "").slice(0, 200);
          const entityKind = String(entity?.entity_kind ?? entity?.entityKind ?? entity?.type ?? entity?.kind ?? "").slice(0, 100);
          if (!id || !entityKind) continue;
          const title = typeof entity?.title === "string" ? String(entity.title).slice(0, 200) : undefined;
          const props = safeObject(entity?.props ?? entity ?? {});
          ops.push({ op: "create", id, entity_kind: entityKind, title, props });
        }
        return ops;
      }
      return [];
    };
    const postClientEvent = async (type: string, data: any) => {
      await apiPostSessionUiEvent(
        effectiveBase,
        {
          session_id: sid,
          type,
          client,
          data,
          append_to_session: true,
        },
        daemonAuth,
      );
    };
    const markApplied = (key: string) => {
      appliedUiActionIdsRef.current[key] = Date.now();
      const appliedKeys = Object.keys(appliedUiActionIdsRef.current);
      if (appliedKeys.length <= appliedUiActionLimit) return;
      const items = appliedKeys
        .map((keyValue) => ({ keyValue, ts: appliedUiActionIdsRef.current[keyValue] || 0 }))
        .sort((a, b) => a.ts - b.ts);
      const overflow = items.length - appliedUiActionLimit;
      for (let i = 0; i < overflow; i += 1) {
        delete appliedUiActionIdsRef.current[items[i].keyValue];
      }
    };

    for (const row of actions) {
      const id = typeof row?.id === "number" ? row.id : Number(row?.id ?? NaN);
      if (!Number.isFinite(id)) continue;
      const key = `${sid}::ui_action_id::${id}`;
      if (appliedUiActionIdsRef.current[key]) continue;
      const action = row?.action ?? {};
      const actionType = typeof action?.type === "string" ? action.type : "";
      if (actionType !== "client_rpc" && actionType !== "collab_rpc" && actionType !== "client_probe") continue;
      const toolCallId = typeof row?.tool_call_id === "string" ? String(row.tool_call_id) : "";
      const rpcId = String(action?.rpc_id ?? action?.probe_id ?? toolCallId ?? "").trim();
      if (!rpcId) continue;
      if (ackedRpcIds.has(rpcId)) {
        markApplied(key);
        continue;
      }
      const rpc = action?.rpc ?? action?.probe ?? {};
      const rpcKind = String(rpc?.kind ?? "").trim();
      if (rpcKind !== "entity_apply") continue;
      const autoRunRequested =
        typeof action?.auto_run === "boolean" ? action.auto_run : typeof action?.auto === "boolean" ? action.auto : true;
      if (!autoRunRequested) continue;
      const argsValue = typeof rpc?.args === "object" && rpc?.args ? rpc.args : rpc;
      const ops = entityApplyArgsToOps(argsValue);
      if (!Array.isArray(ops) || ops.length === 0) continue;

      markApplied(key);
      const started = Date.now();
      try {
        const result = applySceneOps(sid, ops);
        void postClientEvent("client_rpc_result", {
          rpc_id: rpcId,
          request_tool_call_id: toolCallId,
          rpc_kind: rpcKind,
          ok: true,
          elapsed_ms: Date.now() - started,
          result,
        }).catch(() => {});
      } catch (error) {
        void postClientEvent("client_rpc_result", {
          rpc_id: rpcId,
          request_tool_call_id: toolCallId,
          rpc_kind: rpcKind,
          ok: false,
          elapsed_ms: Date.now() - started,
          error: String(error),
        }).catch(() => {});
      }
    }
  }, [allowClientEffects, allowClientRpcs, applySceneOps, client, daemonAuth, dbClientEventsData, dbUiActionsData, effectiveBase, sessionId]);

  const artifactAckedRef = React.useRef<Record<string, boolean>>({});
  React.useEffect(() => {
    if (!artifactCatalogSupported) return;
    const sid = String(sessionId || "").trim();
    if (!sid) return;
    const rows =
      sessionArtifactsData?.ok && Array.isArray(sessionArtifactsData?.artifacts) ? (sessionArtifactsData.artifacts as any[]) : [];
    if (rows.length === 0) return;

    const safeString = (value: any) => (typeof value === "string" ? value : "");
    const isAbsoluteLikePath = (path: string) => {
      const trimmed = (path || "").trim();
      if (!trimmed) return false;
      if (trimmed.startsWith("/")) return true;
      if (/^[a-zA-Z]:[\\/]/.test(trimmed)) return true;
      if (trimmed.startsWith("\\\\")) return true;
      return false;
    };
    const post = async (type: string, data: any) => {
      await apiPostSessionUiEvent(
        effectiveBase,
        {
          session_id: sid,
          type,
          client,
          data,
          append_to_session: false,
        },
        daemonAuth,
      );
    };

    rows
      .slice(0, 32)
      .map((record: any) => {
        const data = record?.data ?? {};
        const artifact = data?.artifact ?? record?.artifact ?? {};
        const toolCallId =
          typeof data?.tool_call_id === "string" ? data.tool_call_id : typeof record?.tool_call_id === "string" ? record.tool_call_id : "";
        return { artifact, toolCallId };
      })
      .filter((entry) => entry.toolCallId && entry.artifact && typeof entry.artifact === "object")
      .slice(0, 8)
      .forEach((entry) => {
        const toolCallId = String(entry.toolCallId || "").trim();
        if (!toolCallId) return;
        const key = `${effectiveBase}::${sid}::${toolCallId}`;
        if (artifactAckedRef.current[key]) return;
        artifactAckedRef.current[key] = true;

        const artifact: any = entry.artifact;
        const path = safeString(artifact?.path);
        const resolvedPath = safeString(artifact?.resolved_path);
        const kind = safeString(artifact?.kind);
        const title = safeString(artifact?.title) || path || "artifact";
        const preferredFetchPath = path && !isAbsoluteLikePath(path) ? path : yolo && resolvedPath ? resolvedPath : path;
        const fallbackFetchPath =
          yolo && preferredFetchPath === path && path && !isAbsoluteLikePath(path) && resolvedPath && isAbsoluteLikePath(resolvedPath)
            ? resolvedPath
            : "";

        void (async () => {
          const tryPaths = [preferredFetchPath, fallbackFetchPath].filter((pathValue) => typeof pathValue === "string" && pathValue.trim().length > 0);
          let lastErr: any = null;
          for (const pathValue of tryPaths) {
            const sidQuery = sid ? `&session_id=${encodeURIComponent(sid)}` : "";
            const src = `${effectiveBase}/api/v1/file?path=${encodeURIComponent(pathValue)}&yolo=${yolo ? "1" : "0"}${sidQuery}`;
            try {
              const resp = await fetch(src, daemonFetchInit(daemonAuth));
              if (!resp.ok) throw new Error(`file fetch failed: ${resp.status}`);
              const contentType = String(resp.headers.get("content-type") || "").trim();
              await resp.arrayBuffer();
              await post("artifact_rendered", {
                path,
                resolved_path: resolvedPath || undefined,
                fetch_path: pathValue,
                kind,
                title,
                tool_call_id: toolCallId,
                content_type: contentType || undefined,
              });
              return;
            } catch (error) {
              lastErr = error;
            }
          }
          await post("artifact_render_failed", {
            path,
            resolved_path: resolvedPath || undefined,
            fetch_path: preferredFetchPath || undefined,
            kind,
            title,
            tool_call_id: toolCallId,
            error: String(lastErr || "failed"),
          });
        })().catch(() => {});
      });
  }, [artifactCatalogSupported, client, daemonAuth, effectiveBase, sessionArtifactsData, sessionId, yolo]);

  return {
    applySceneOps,
    sceneEntities,
  };
}
