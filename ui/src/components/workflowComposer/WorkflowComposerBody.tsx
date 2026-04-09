import React from "react";

import WorkflowGraphComposer from "../WorkflowGraphComposer";
import type { WorkflowDefaults } from "../../workflowTypes";
import type { ComposerMode, GraphSetter, TemplateKind, WorkflowComposerGraphBuild } from "./workflowComposerTypes";
import type { GraphState } from "./workflowComposerUtils";

type WorkflowComposerBodyProps = {
  allowInlineKeys: boolean;
  bearerEnv?: string;
  composerJson: string;
  composerMode: ComposerMode;
  defaults: WorkflowDefaults;
  graphBuild: WorkflowComposerGraphBuild;
  graphParseWarnings: string[];
  graphState: GraphState;
  submitError: string | null;
  templateKind: TemplateKind;
  onClearWarnings: () => void;
  onImportJson: () => void;
  onExportJson: () => void;
  onSetAllowInlineKeys: (next: boolean) => void;
  onSetComposerJson: (next: string) => void;
  onSetGraphState: GraphSetter;
};

export default function WorkflowComposerBody(props: WorkflowComposerBodyProps) {
  return (
    <>
      <div className="mt-2 grid gap-2 text-[11px] text-white/60">
        {props.composerMode === "json" ? (
          <div>
            Templates are read-only helpers. Edit the JSON below before submitting.
            {props.templateKind === "agent_parallel" ? (
              <span className="text-amber-200"> Requires `--workflow-enable-http-tasks` on the primary agentd.</span>
            ) : null}
          </div>
        ) : (
          <div>Graph editor supports LLM and agentd_parallel tasks. Use JSON mode for advanced workflow kinds.</div>
        )}
        {props.defaults.api_key ? (
          <label className="flex items-center gap-1">
            <input
              type="checkbox"
              className="h-3 w-3"
              checked={props.allowInlineKeys}
              onChange={(event) => props.onSetAllowInlineKeys(event.target.checked)}
            />
            allow inline API keys (stored in DB)
          </label>
        ) : null}
        {props.composerMode === "json" && props.templateKind === "agent_parallel" && !props.bearerEnv ? (
          <div className="rounded-md border border-amber-500/30 bg-amber-500/10 px-2 py-1 text-xs text-amber-100">
            No bearer env configured. If remote agents require auth, set a bearer env in UI runtime config
            (e.g. <code className="text-amber-100">workflowBearerEnv</code>).
          </div>
        ) : null}
      </div>

      {props.composerMode === "graph" ? (
        <WorkflowGraphComposer
          state={props.graphState}
          onChange={props.onSetGraphState}
          buildResult={props.graphBuild.result}
          buildError={props.graphBuild.error}
          parseWarnings={props.graphParseWarnings}
          onImportJson={props.onImportJson}
          onExportJson={props.onExportJson}
          bearerEnv={props.bearerEnv}
          onClearWarnings={props.onClearWarnings}
        />
      ) : (
        <textarea
          className="mt-3 h-64 w-full rounded-md border border-white/10 bg-black/40 p-2 font-mono text-[11px] text-white/80"
          data-testid="workflow-composer-json"
          value={props.composerJson}
          onChange={(event) => props.onSetComposerJson(event.target.value)}
          placeholder='Paste workflow JSON here. Use "Apply" to load a template.'
        />
      )}

      {props.submitError && props.composerMode === "graph" ? (
        <div className="mt-2 text-xs text-rose-200">{props.submitError}</div>
      ) : null}
    </>
  );
}
