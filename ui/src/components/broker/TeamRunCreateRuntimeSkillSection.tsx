import React from "react";

import WorkflowComposerRuntimeSkillSection, {
  type WorkflowComposerRuntimeSkillSectionProps,
} from "../workflowComposer/WorkflowComposerRuntimeSkillSection";

type TeamRunCreateRuntimeSkillSectionProps = Omit<
  WorkflowComposerRuntimeSkillSectionProps,
  "applyLabel" | "emptyStateCopy" | "loadingCopy" | "sectionDescription" | "sectionTitle"
>;

export default function TeamRunCreateRuntimeSkillSection(props: TeamRunCreateRuntimeSkillSectionProps) {
  return (
    <WorkflowComposerRuntimeSkillSection
      {...props}
      applyLabel="Apply to run"
      emptyStateCopy="Select a team bundle to materialize it into the team-run request."
      loadingCopy="Loading runtime skills…"
      sectionDescription="Start from a reusable team bundle instead of filling every team-run field by hand."
      sectionTitle="Runtime skill"
    />
  );
}
