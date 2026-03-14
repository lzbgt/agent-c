import React from "react";

import WorkflowComposerBody from "./workflowComposer/WorkflowComposerBody";
import WorkflowComposerStatus from "./workflowComposer/WorkflowComposerStatus";
import WorkflowComposerToolbar from "./workflowComposer/WorkflowComposerToolbar";
import type { WorkflowComposerProps } from "./workflowComposer/workflowComposerTypes";
import useWorkflowComposerState from "./workflowComposer/useWorkflowComposerState";

export type { WorkflowComposerProps } from "./workflowComposer/workflowComposerTypes";

export default function WorkflowComposer(props: WorkflowComposerProps) {
  const state = useWorkflowComposerState(props);

  return (
    <div className="rounded-md border border-white/10 bg-black/30 p-3">
      <WorkflowComposerToolbar
        composerMode={state.composerMode}
        templateKind={state.templateKind}
        submitBusy={state.submitBusy}
        onSetComposerMode={state.setComposerMode}
        onApplyTemplate={state.applyTemplate}
        onSubmitDemoAndWait={state.submitDemoAndWait}
        onFormatJson={state.formatJson}
      />

      <WorkflowComposerBody
        allowInlineKeys={state.allowInlineKeys}
        bearerEnv={state.bearerEnv}
        composerJson={state.composerJson}
        composerMode={state.composerMode}
        defaults={state.defaults}
        graphBuild={state.graphBuild}
        graphParseWarnings={state.graphParseWarnings}
        graphState={state.graphState}
        submitError={state.submitError}
        templateKind={state.templateKind}
        onClearWarnings={state.clearGraphWarnings}
        onImportJson={state.importGraphFromJson}
        onExportJson={state.exportGraphToJson}
        onSetAllowInlineKeys={state.setAllowInlineKeys}
        onSetComposerJson={state.setComposerJson}
        onSetGraphState={state.setGraphState}
      />

      <WorkflowComposerStatus
        cancelBusy={state.cancelBusy}
        serverWaitStatus={state.serverWaitStatus}
        submitBusy={state.submitBusy}
        submitError={state.submitError}
        submitResult={state.submitResult}
        waitPersisted={state.waitPersisted}
        waitPersistedExtra={state.waitPersistedExtra}
        waitState={state.waitState}
        onCancelWorkflow={state.cancelWorkflow}
        onClearPersistedWait={state.clearPersistedWait}
        onResumePersistedWait={state.resumePersistedWait}
        onSubmit={state.submit}
      />
    </div>
  );
}
