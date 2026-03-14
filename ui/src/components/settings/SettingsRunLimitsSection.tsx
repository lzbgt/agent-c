import React from "react";
import type { RunSettings } from "../../hooks/useUiSettings";
import FieldLabel from "../FieldLabel";

type SettingsRunLimitsSectionProps = {
  run: RunSettings;
};

export default function SettingsRunLimitsSection(props: SettingsRunLimitsSectionProps) {
  const { run } = props;

  return (
    <details className="mt-4 rounded-md border border-white/10 bg-black/20 p-3">
      <summary className="cursor-pointer text-xs font-semibold text-white/70">Run limits</summary>
      <div className="mt-3 grid gap-3 text-[11px] text-white/70">
        <div className="grid grid-cols-2 gap-3">
          <div>
            <FieldLabel>Max steps</FieldLabel>
            <input
              className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
              value={run.maxSteps}
              onChange={(e) => run.setMaxSteps(e.target.value)}
              placeholder="blank = daemon default"
            />
          </div>
          <div>
            <FieldLabel>Max repeated tool calls</FieldLabel>
            <input
              className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
              value={run.maxRepeatedToolCalls}
              onChange={(e) => run.setMaxRepeatedToolCalls(e.target.value)}
              inputMode="numeric"
            />
          </div>
        </div>
        <div className="grid grid-cols-2 gap-3">
          <div>
            <FieldLabel>Max tool calls total</FieldLabel>
            <input
              className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
              value={run.maxToolCallsTotal}
              onChange={(e) => run.setMaxToolCallsTotal(e.target.value)}
              placeholder="blank = daemon default"
            />
          </div>
          <div>
            <FieldLabel>Max tool calls per tool</FieldLabel>
            <input
              className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
              value={run.maxToolCallsPerTool}
              onChange={(e) => run.setMaxToolCallsPerTool(e.target.value)}
              placeholder="blank = daemon default"
            />
          </div>
        </div>
        <div>
          <FieldLabel>Tool call limits</FieldLabel>
          <textarea
            className="mt-1 min-h-[90px] w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
            value={run.toolCallLimits}
            onChange={(e) => run.setToolCallLimits(e.target.value)}
            placeholder="tool=max_calls (comma or newline separated) or JSON list"
          />
        </div>
        <div className="grid grid-cols-2 gap-3">
          <div>
            <FieldLabel>Max chars</FieldLabel>
            <input
              className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
              value={run.maxChars}
              onChange={(e) => run.setMaxChars(e.target.value)}
              inputMode="numeric"
            />
          </div>
          <div>
            <FieldLabel>Keep last</FieldLabel>
            <input
              className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
              value={run.keepLast}
              onChange={(e) => run.setKeepLast(e.target.value)}
              inputMode="numeric"
            />
          </div>
        </div>
      </div>
    </details>
  );
}
