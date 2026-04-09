import React from "react";

import { apiPostSessionUiEvent, type ApiAuth } from "../../api";
import useLocalStorageState from "../../hooks/useLocalStorageState";
import type { SceneClientRef, SceneEntity } from "./sceneViewTypes";

type UseSceneViewStateArgs = {
  baseUrl?: string;
  client?: SceneClientRef;
  daemonAuth?: ApiAuth;
  entities: SceneEntity[];
  sessionId?: string;
};

export default function useSceneViewState(args: UseSceneViewStateArgs) {
  const sid = typeof args.sessionId === "string" ? args.sessionId.trim() : "";
  const lastSceneErrorRef = React.useRef<Record<string, { ts: number; sig: string }>>({});
  const defaultExpandedCount = 1;

  const sortedEntities = React.useMemo(() => {
    const copy = [...args.entities];
    copy.sort((a, b) => {
      const ta = typeof a.updated_ms === "number" ? a.updated_ms : typeof a.created_ms === "number" ? a.created_ms : 0;
      const tb = typeof b.updated_ms === "number" ? b.updated_ms : typeof b.created_ms === "number" ? b.created_ms : 0;
      if (ta !== tb) return tb - ta;
      return String(b.id || "").localeCompare(String(a.id || ""));
    });
    return copy;
  }, [args.entities]);

  const expandedKey = React.useMemo(() => {
    const base = typeof args.baseUrl === "string" ? args.baseUrl.trim() : "";
    const sidKey = typeof args.sessionId === "string" ? args.sessionId.trim() : "";
    return `agentui.scene.expandedById:${base}::${sidKey}`;
  }, [args.baseUrl, args.sessionId]);
  const [expandedById, setExpandedById] = useLocalStorageState<Record<string, boolean>>(expandedKey, {});

  const showAllEntitiesKey = React.useMemo(() => {
    const base = typeof args.baseUrl === "string" ? args.baseUrl.trim() : "";
    const sidKey = typeof args.sessionId === "string" ? args.sessionId.trim() : "";
    return `agentui.scene.showAllEntities:${base}::${sidKey}`;
  }, [args.baseUrl, args.sessionId]);
  const [showAllEntities, setShowAllEntities] = useLocalStorageState<boolean>(showAllEntitiesKey, false);

  React.useEffect(() => {
    setExpandedById((prev) => {
      const next: Record<string, boolean> = {};
      for (let i = 0; i < sortedEntities.length; i++) {
        const id = String(sortedEntities[i]?.id || "");
        if (!id) continue;
        if (Object.prototype.hasOwnProperty.call(prev, id)) next[id] = !!prev[id];
        else next[id] = i < defaultExpandedCount;
      }
      return next;
    });
  }, [defaultExpandedCount, setExpandedById, sortedEntities, sid]);

  const postSceneError = React.useCallback(
    async (payload: { entity_id: string; entity_kind: string; error: string; stack_preview?: string; script_preview?: string }) => {
      if (!args.baseUrl) return;
      if (!sid) return;
      const cid = args.client && typeof args.client === "object" ? args.client : { id: "webui", kind: "webui" };
      const sig = `${payload.entity_id}:${payload.error}`;
      const now = Date.now();
      const prev = lastSceneErrorRef.current[payload.entity_id];
      if (prev && prev.sig === sig && now - prev.ts < 10_000) return;
      lastSceneErrorRef.current[payload.entity_id] = { ts: now, sig };

      try {
        await apiPostSessionUiEvent(
          args.baseUrl,
          {
            session_id: sid,
            type: "scene_error",
            client: cid,
            data: { ...payload, ts_unix_ms: now },
            append_to_session: false,
          },
          args.daemonAuth,
        );
      } catch {
        // ignore
      }
    },
    [args.baseUrl, args.client, args.daemonAuth, sid],
  );

  const visibleEntities = showAllEntities ? sortedEntities : sortedEntities.slice(0, defaultExpandedCount);
  const historyEntitiesCount = Math.max(0, sortedEntities.length - defaultExpandedCount);
  const hiddenEntitiesCount = Math.max(0, sortedEntities.length - visibleEntities.length);

  return {
    defaultExpandedCount,
    expandedById,
    historyEntitiesCount,
    hiddenEntitiesCount,
    postSceneError,
    setExpandedById,
    setShowAllEntities,
    showAllEntities,
    sid,
    sortedEntities,
    visibleEntities,
  };
}
