import React from "react";

import type { ApiAuth } from "../../api";
import ArtifactView from "../ArtifactView";
import Canvas2DEntityView from "./Canvas2DEntityView";
import DomEntityView from "./DomEntityView";
import Markdown from "../Markdown";
import type { SceneClientRef, SceneEntity, SceneScriptErrorArgs } from "./sceneViewTypes";
import { toTestIdPart } from "./sceneViewUtils";

function JsonEntityView({ entity }: { entity: SceneEntity }) {
  return (
    <details className="mt-2 text-xs text-white/60">
      <summary className="cursor-pointer">Result data</summary>
    <pre className="mt-2 overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/20 p-2 text-xs text-white/80">
      {JSON.stringify(entity.props ?? {}, null, 2)}
    </pre>
    </details>
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

  return (
    <div key={entity.id} className="rounded-md border border-white/10 bg-black/10 p-3" data-testid={entityTid}>
      <button
        className="flex w-full items-center justify-between gap-2 text-left"
        type="button"
        onClick={onToggleExpanded}
        title={expanded ? "Collapse" : "Expand"}
      >
        <div className="text-xs font-semibold text-white/80">{title}</div>
        <span className="text-xs text-white/40" aria-hidden="true">{expanded ? "−" : "+"}</span>
      </button>

      {!expanded ? (
        <div className="mt-2 text-[11px] text-white/40">Collapsed</div>
      ) : ["note", "text", "markdown"].includes(entity.kind) && typeof (props.text ?? props.markdown ?? props.content) === "string" ? (
        <div className="mt-3"><Markdown text={String(props.text ?? props.markdown ?? props.content)} /></div>
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
