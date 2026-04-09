import React from "react";

import type { ApiAuth } from "../../api";
import ArtifactView from "../ArtifactView";
import Canvas2DEntityView from "./Canvas2DEntityView";
import DomEntityView from "./DomEntityView";
import type { SceneClientRef, SceneEntity, SceneScriptErrorArgs } from "./sceneViewTypes";
import { toTestIdPart } from "./sceneViewUtils";

function JsonEntityView({ entity }: { entity: SceneEntity }) {
  return (
    <pre className="mt-2 overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/20 p-2 text-xs text-white/80">
      {JSON.stringify(entity.props ?? {}, null, 2)}
    </pre>
  );
}

export default function SceneEntityCard({
  allowAutoplay,
  baseUrl,
  client,
  daemonAuth,
  defaultExpandedCount,
  entity,
  expanded,
  idx,
  onScriptError,
  onToggleExpanded,
  sessionId,
  yolo,
}: {
  allowAutoplay?: boolean;
  baseUrl?: string;
  client?: SceneClientRef;
  daemonAuth?: ApiAuth;
  defaultExpandedCount: number;
  entity: SceneEntity;
  expanded: boolean;
  idx: number;
  onScriptError?: (args: SceneScriptErrorArgs) => void;
  onToggleExpanded: () => void;
  sessionId?: string;
  yolo?: boolean;
}) {
  const title = entity.title || `${entity.kind}:${entity.id}`;
  const entityTid = `scene-entity-${toTestIdPart(entity.id)}`;
  const props = entity.props ?? {};
  const ts = typeof entity.updated_ms === "number" ? entity.updated_ms : typeof entity.created_ms === "number" ? entity.created_ms : 0;

  return (
    <div key={entity.id} className="rounded-md border border-white/10 bg-black/10 p-3" data-testid={entityTid}>
      <button
        className="flex w-full items-center justify-between gap-2 text-left"
        type="button"
        onClick={onToggleExpanded}
        title={expanded ? "Collapse" : "Expand"}
      >
        <div className="text-xs font-semibold text-white/80">{title}</div>
        <div className="flex items-center gap-2 text-[11px] text-white/40">
          {ts > 0 ? <span>{new Date(ts).toLocaleString()}</span> : null}
          <code>{entity.id}</code>
        </div>
      </button>

      {!expanded ? (
        <div className="mt-2 text-[11px] text-white/40">Collapsed</div>
      ) : entity.kind === "canvas2d" ? (
        <Canvas2DEntityView
          entity={entity}
          baseUrl={baseUrl}
          yolo={yolo}
          sessionId={sessionId}
          daemonAuth={daemonAuth}
          onScriptError={onScriptError}
        />
      ) : entity.kind === "dom" ? (
        <DomEntityView
          entity={entity}
          baseUrl={baseUrl}
          yolo={yolo}
          allowAutoplay={allowAutoplay}
          sessionId={sessionId}
          daemonAuth={daemonAuth}
          onScriptError={onScriptError}
        />
      ) : entity.kind === "artifact" && baseUrl && typeof yolo === "boolean" ? (
        <div className="mt-2">
          <ArtifactView
            baseUrl={baseUrl}
            yolo={yolo}
            artifact={props?.artifact ?? props}
            allowAutoplay={!!allowAutoplay}
            sessionId={sessionId}
            client={client}
            daemonAuth={daemonAuth}
          />
        </div>
      ) : (
        <JsonEntityView entity={entity} />
      )}
    </div>
  );
}
