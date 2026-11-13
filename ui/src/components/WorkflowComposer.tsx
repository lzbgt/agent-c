import React from "react";

import WorkflowComposerBody from "./workflowComposer/WorkflowComposerBody";
import WorkflowSopLibrary from "./workflowComposer/WorkflowSopLibrary";
import WorkflowComposerRuntimeSkillSection from "./workflowComposer/WorkflowComposerRuntimeSkillSection";
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

      <WorkflowSopLibrary
        key={`${props.baseUrl}::${props.authKey || ""}`}
        baseUrl={props.baseUrl}
        auth={props.auth}
        composerJson={state.composerMode === "json" ? state.composerJson : ""}
        disabled={state.submitBusy}
        onUse={(sop_ref) => {
          state.setComposerMode("json");
          state.setComposerJson(JSON.stringify({ sop_ref }, null, 2));
        }}
      />

      <details className="mt-4 text-xs text-white/60">
        <summary className="flex cursor-pointer items-center">Use a runtime skill</summary>
        <WorkflowComposerRuntimeSkillSection
          applyBusy={state.runtimeSkillApplyBusy}
          inputsJson={state.runtimeSkillInputsJson}
          inputsParseError={state.runtimeSkillInputsParseError}
          loadError={state.runtimeSkillListError}
          loading={state.runtimeSkillListLoading}
          runtimeSkillError={state.runtimeSkillError}
          selectedSkill={state.selectedRuntimeSkill}
          selectedSkillId={state.runtimeSkillId}
          skills={state.runtimeSkills}
          onApply={() => void state.applyRuntimeSkill()}
          onSelectSkill={state.selectRuntimeSkill}
          onSetInputsJson={state.setRuntimeSkillInputsJson}
        />

      </details>

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

      {state.pendingSubmission ? <section className="mt-3 rounded border border-amber-400/30 p-3 text-sm" aria-label="Unresolved submission">
        <p>An earlier submission has no confirmed outcome. Check the original receipt rather than creating another run.</p>
        <p className="break-all text-xs">Receipt key: {state.pendingSubmission.idempotency_key}</p>
        <button type="button" className="mt-2 rounded border border-white/20 px-3 py-2 disabled:opacity-40" disabled={state.submitBusy} onClick={() => void state.reconcileSubmission()}>Check submission receipt</button>
        <button type="button" className="ml-2 mt-2 rounded border border-white/20 px-3 py-2 disabled:opacity-40" disabled={state.submitBusy} onClick={state.forgetSubmission}>Forget local receipt…</button>
      </section> : null}
      <WorkflowComposerStatus
        cancelBusy={state.cancelBusy}
        serverWaitStatus={state.serverWaitStatus}
        submitBusy={state.submitBusy}
        submitBlocked={state.submissionBlocked}
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
