import RunDiffEvidenceSection from "./runDiff/RunDiffEvidenceSection";
import RunDiffLoadersSection from "./runDiff/RunDiffLoadersSection";
import RunDiffReplaySection from "./runDiff/RunDiffReplaySection";
import RunDiffMetadata from "./runDiff/RunDiffMetadata";
import RunDiffOutputs from "./runDiff/RunDiffOutputs";
import { useRunDiffPanelState } from "./runDiff/useRunDiffPanelState";
import type { ApiAuth } from "../api";

export type RunDiffPanelProps = {
  baseUrl: string;
  auth: ApiAuth;
};

export default function RunDiffPanel(props: RunDiffPanelProps) {
  const state = useRunDiffPanelState({ baseUrl: props.baseUrl, auth: props.auth });

  return (
    <section
      className="tool-workspace"
      data-testid="run-diff-panel"
    >
      <div className="mt-3 grid gap-3">
        <RunDiffLoadersSection
          canQuery={state.canQuery}
          baselineRunId={state.baselineRunId}
          setBaselineFromA={state.setBaselineFromA}
          useBaselineForB={state.useBaselineForB}
          sideA={state.sideA}
          sideB={state.sideB}
        />
        <RunDiffOutputs sideA={state.sideA} sideB={state.sideB} dbDiff={state.dbDiff} />
        <details className="tool-details tool-maintenance" data-testid="run-diff-details">
          <summary>Execution details</summary>
          <RunDiffMetadata sideA={state.sideA} sideB={state.sideB} replayDiff={state.replayDiff} dbDiff={state.dbDiff} sameReplayHash={state.sameReplayHash} />
        <RunDiffReplaySection
          diffOnly={state.diffOnly}
          setDiffOnly={state.setDiffOnly}
          replayDiff={state.replayDiff}
          sideA={state.sideA}
          sideB={state.sideB}
        />
        <RunDiffEvidenceSection
          dbDiff={state.dbDiff}
          diffOnly={state.diffOnly}
          sideA={state.sideA}
          sideB={state.sideB}
        />
        </details>
      </div>
    </section>
  );
}
