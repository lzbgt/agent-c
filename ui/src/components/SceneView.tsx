import React from "react";

import SceneEntityCard from "./scene/SceneEntityCard";
import useSceneViewState from "./scene/useSceneViewState";
import type { SceneEntity, SceneViewProps } from "./scene/sceneViewTypes";

export type { SceneEntity } from "./scene/sceneViewTypes";

export default function SceneView({ baseUrl, yolo, allowAutoplay, client, daemonAuth, sessionId, entities, className }: SceneViewProps) {
  const state = useSceneViewState({ baseUrl, client, daemonAuth, entities, sessionId });

  return (
    <div
      className={["flex min-h-0 flex-col rounded-lg border border-white/10 bg-white/5", className].filter(Boolean).join(" ")}
      data-testid="scene"
    >
      <div className="flex shrink-0 items-center justify-between px-3 py-2">
        <div className="text-sm font-semibold" data-testid="scene-header">
          Scene
        </div>
        <div className="flex items-center gap-2">
          <div className="text-[11px] text-white/40" data-testid="scene-session">
            {state.sid ? `session=${state.sid}` : ""}
          </div>
        </div>
      </div>

      <div className="min-h-0 flex-1 overflow-auto px-3 pb-3">
        {entities.length === 0 ? (
          <div className="py-6 text-xs text-white/50">No scene entities.</div>
        ) : (
          <div className="space-y-3">
            <div className="flex flex-wrap items-center justify-between gap-2">
              <div className="text-[11px] text-white/40">
                {state.showAllEntities ? (
                  <>Showing all {state.sortedEntities.length} entities.</>
                ) : (
                  <>
                    Showing latest {Math.min(state.defaultExpandedCount, state.sortedEntities.length)}; {state.hiddenEntitiesCount} hidden.
                  </>
                )}
              </div>
              <div className="flex items-center gap-2">
                {state.historyEntitiesCount > 0 ? (
                  <button
                    className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
                    data-testid="scene-history-toggle"
                    type="button"
                    onClick={() => state.setShowAllEntities((v) => !v)}
                    title={state.showAllEntities ? "Hide older entities" : "Show older entities"}
                  >
                    {state.showAllEntities
                      ? `Hide history (${state.historyEntitiesCount})`
                      : `Show history (${state.historyEntitiesCount})`}
                  </button>
                ) : null}
                {state.showAllEntities ? (
                  <>
                    <button
                      className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                      data-testid="scene-expand-all"
                      type="button"
                      onClick={() =>
                        state.setExpandedById((prev) => {
                          const next = { ...prev };
                          for (const entity of state.sortedEntities) next[entity.id] = true;
                          return next;
                        })
                      }
                      disabled={state.sortedEntities.length === 0}
                      title="Expand all scene entities."
                    >
                      Expand all
                    </button>
                    <button
                      className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                      data-testid="scene-collapse-all"
                      type="button"
                      onClick={() =>
                        state.setExpandedById((prev) => {
                          const next = { ...prev };
                          for (const entity of state.sortedEntities) next[entity.id] = false;
                          return next;
                        })
                      }
                      disabled={state.sortedEntities.length === 0}
                      title="Collapse all scene entities."
                    >
                      Collapse all
                    </button>
                  </>
                ) : null}
              </div>
            </div>

            {state.visibleEntities.map((entity: SceneEntity, idx: number) => {
              const expanded = Object.prototype.hasOwnProperty.call(state.expandedById, entity.id)
                ? !!state.expandedById[entity.id]
                : idx < state.defaultExpandedCount;
              return (
                <SceneEntityCard
                  key={entity.id}
                  allowAutoplay={allowAutoplay}
                  baseUrl={baseUrl}
                  client={client}
                  daemonAuth={daemonAuth}
                  defaultExpandedCount={state.defaultExpandedCount}
                  entity={entity}
                  expanded={expanded}
                  idx={idx}
                  onScriptError={state.postSceneError}
                  onToggleExpanded={() => state.setExpandedById((prev) => ({ ...prev, [entity.id]: !expanded }))}
                  sessionId={sessionId}
                  yolo={yolo}
                />
              );
            })}
          </div>
        )}
      </div>
    </div>
  );
}
