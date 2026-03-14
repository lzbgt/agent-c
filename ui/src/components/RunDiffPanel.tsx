import RunDiffEvidenceSection from "./runDiff/RunDiffEvidenceSection";
import RunDiffLoadersSection from "./runDiff/RunDiffLoadersSection";
import RunDiffReplaySection from "./runDiff/RunDiffReplaySection";
import { useRunDiffPanelState } from "./runDiff/useRunDiffPanelState";
import type { ApiAuth } from "../api";

export type RunDiffPanelProps = {
  open: boolean;
  onToggle: (open: boolean) => void;
  baseUrl: string;
  auth: ApiAuth;
};

export default function RunDiffPanel(props: RunDiffPanelProps) {
  const state = useRunDiffPanelState({ baseUrl: props.baseUrl, auth: props.auth });

  return (
    <details
      className="mb-4 rounded-lg border border-white/10 bg-white/5 px-3 py-2"
      open={!!props.open}
      onToggle={(ev) => props.onToggle((ev.currentTarget as HTMLDetailsElement).open)}
      data-testid="run-diff-panel"
    >
      <summary className="cursor-pointer select-none text-xs text-white/80">
        <div className="flex flex-wrap items-center justify-between gap-2">
          <div className="font-semibold text-white/80">Run diff</div>
          <div className="text-[11px] text-white/50">Compare run replay bundles (request/response/tool records)</div>
        </div>
      </summary>
      <div className="mt-3 grid gap-3">
        <RunDiffLoadersSection
          canQuery={state.canQuery}
          baselineRunId={state.baselineRunId}
          setBaselineFromA={state.setBaselineFromA}
          useBaselineForB={state.useBaselineForB}
          sideA={state.sideA}
          sideB={state.sideB}
          replayDiff={state.replayDiff}
          dbDiff={state.dbDiff}
          sameReplayHash={state.sameReplayHash}
        />
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
      </div>
    </details>
  );
}
