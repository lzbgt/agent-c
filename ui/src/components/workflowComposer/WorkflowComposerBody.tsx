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
      {props.defaults.api_key ? <details className="tool-details"><summary>Credential options</summary>
        <label className="tool-checks"><input type="checkbox" checked={props.allowInlineKeys} onChange={(event) => props.onSetAllowInlineKeys(event.target.checked)} />Store provider key with this workflow</label>
      </details> : null}

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
          aria-label="Workflow definition"
        />
      )}

      {props.submitError && props.composerMode === "graph" ? (
        <div className="mt-2 text-xs text-rose-200">{props.submitError}</div>
      ) : null}
    </>
  );
}
