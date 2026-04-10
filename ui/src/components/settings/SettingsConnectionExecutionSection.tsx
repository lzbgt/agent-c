import FieldLabel from "../FieldLabel";
import type { SettingsConnectionSectionProps } from "./settingsConnectionTypes";

type SettingsConnectionExecutionSectionProps = Pick<
  SettingsConnectionSectionProps,
  "run" | "automationProfiles" | "automationDefault" | "automationOverrideAllowed"
>;

function isToolMode(value: string): value is SettingsConnectionSectionProps["run"]["tools"] {
  return value === "host" || value === "basic" || value === "none";
}

function isHostPolicy(value: string): value is SettingsConnectionSectionProps["run"]["hostPolicy"] {
  return value === "full" || value === "readonly";
}

export default function SettingsConnectionExecutionSection(props: SettingsConnectionExecutionSectionProps) {
  const { run, automationProfiles, automationDefault, automationOverrideAllowed } = props;

  return (
    <>
      <div>
        <FieldLabel>Tools</FieldLabel>
        <select
          className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
          value={run.tools}
          onChange={(e) => {
            const next = e.target.value;
            if (isToolMode(next)) run.setTools(next);
          }}
        >
          <option value="host">host</option>
          <option value="basic">basic</option>
          <option value="none">none</option>
        </select>
      </div>
      <div>
        <FieldLabel>Host policy</FieldLabel>
        <select
          className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
          value={run.hostPolicy}
          onChange={(e) => {
            const next = e.target.value;
            if (isHostPolicy(next)) run.setHostPolicy(next);
          }}
          disabled={run.tools !== "host"}
        >
          <option value="full">full</option>
          <option value="readonly">readonly</option>
        </select>
      </div>
      <div className="col-span-2">
        <FieldLabel>Automation profile</FieldLabel>
        <select
          className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
          value={run.automationProfile}
          onChange={(e) => run.setAutomationProfile(e.target.value)}
          disabled={!automationOverrideAllowed}
        >
          <option value="">{automationDefault ? `default (${automationDefault})` : "default (daemon config)"}</option>
          {automationProfiles.map((p) => (
            <option key={p} value={p}>
              {p}
            </option>
          ))}
        </select>
        <div className="mt-1 text-[11px] text-white/60">
          Overrides yolo/host policy/policy mode when set. Use default to follow daemon config.
        </div>
        {!automationOverrideAllowed ? (
          <div className="mt-1 text-[11px] text-amber-200">Per-run automation override disabled by daemon caps.</div>
        ) : null}
      </div>
    </>
  );
}
