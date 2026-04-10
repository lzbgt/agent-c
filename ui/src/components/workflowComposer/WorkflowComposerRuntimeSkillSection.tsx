import React from "react";

import type {
  RuntimeSkillInputProperty,
  RuntimeSkillInputsSchema,
  RuntimeSkillSummary,
} from "../../api/schemas/runtime_skills";

type WorkflowComposerRuntimeSkillSectionProps = {
  applyBusy: boolean;
  inputsJson: string;
  inputsParseError: string | null;
  loadError: string | null;
  loading: boolean;
  runtimeSkillError: string | null;
  selectedSkill: RuntimeSkillSummary | null;
  selectedSkillId: string;
  skills: RuntimeSkillSummary[];
  onApply: () => void;
  onSelectSkill: (skillId: string) => void;
  onSetInputsJson: (next: string) => void;
};

const isObjectRecord = (value: unknown): value is Record<string, unknown> =>
  !!value && typeof value === "object" && !Array.isArray(value);

const toPrettyJson = (value: Record<string, unknown>) => JSON.stringify(value, null, 2);

const schemaProperties = (schema: RuntimeSkillInputsSchema | null | undefined) => {
  const props = schema?.properties;
  if (!props || typeof props !== "object") return [];
  return Object.entries(props);
};

const requiredKeys = (schema: RuntimeSkillInputsSchema | null | undefined) =>
  new Set(Array.isArray(schema?.required) ? schema.required : []);

function coercePrimitiveInput(raw: string, property: RuntimeSkillInputProperty): unknown {
  const type = property.type || "string";
  if (type === "integer") {
    return Number.parseInt(raw, 10);
  }
  if (type === "number") {
    return Number(raw);
  }
  return raw;
}

export default function WorkflowComposerRuntimeSkillSection(props: WorkflowComposerRuntimeSkillSectionProps) {
  const selectedSchema = props.selectedSkill?.inputs_schema;
  const generatedProperties = schemaProperties(selectedSchema);
  const required = requiredKeys(selectedSchema);

  let parsedInputs: Record<string, unknown> = {};
  if (!props.inputsParseError) {
    try {
      const parsed = JSON.parse(props.inputsJson || "{}");
      if (isObjectRecord(parsed)) parsedInputs = parsed;
    } catch {
      // ignored: error is computed by caller
    }
  }

  const updateInputsField = (key: string, nextValue: unknown, opts?: { removeIfBlank?: boolean }) => {
    const next = { ...parsedInputs };
    if (opts?.removeIfBlank && typeof nextValue === "string" && !nextValue.trim()) {
      delete next[key];
    } else if (nextValue === undefined) {
      delete next[key];
    } else {
      next[key] = nextValue;
    }
    props.onSetInputsJson(toPrettyJson(next));
  };

  return (
    <div className="mt-3 rounded-md border border-white/10 bg-black/20 p-3">
      <div className="flex flex-wrap items-center justify-between gap-2">
        <div>
          <div className="text-xs font-semibold text-white/70">Runtime skill</div>
          <div className="text-[11px] text-white/50">
            Start from a reusable workflow bundle instead of hand-authoring the JSON.
          </div>
        </div>
        <div className="flex flex-wrap items-center gap-2 text-[11px] text-white/60">
          <label className="flex items-center gap-1">
            skill
            <select
              className="rounded border border-white/10 bg-black/40 px-1 py-0.5 text-[11px] text-white/80"
              value={props.selectedSkillId}
              onChange={(event) => props.onSelectSkill(event.target.value)}
              disabled={props.loading || props.skills.length === 0}
            >
              <option value="">Select runtime skill…</option>
              {props.skills.map((skill) => (
                <option key={skill.skill_id} value={skill.skill_id}>
                  {skill.label || skill.skill_id}
                </option>
              ))}
            </select>
          </label>
          <button
            type="button"
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40 disabled:opacity-50"
            onClick={props.onApply}
            disabled={!props.selectedSkill || props.applyBusy}
          >
            {props.applyBusy ? "Applying…" : "Apply skill"}
          </button>
        </div>
      </div>

      {props.loading ? <div className="mt-2 text-xs text-white/50">Loading runtime skills…</div> : null}
      {props.loadError ? <div className="mt-2 text-xs text-rose-200">{props.loadError}</div> : null}
      {props.runtimeSkillError ? <div className="mt-2 text-xs text-rose-200">{props.runtimeSkillError}</div> : null}

      {props.selectedSkill ? (
        <>
          <div className="mt-2 text-xs text-white/65">
            <span className="font-medium text-white/80">{props.selectedSkill.label || props.selectedSkill.skill_id}</span>
            {props.selectedSkill.category ? <span className="text-white/45"> · {props.selectedSkill.category}</span> : null}
          </div>
          <div className="mt-1 text-xs text-white/55">{props.selectedSkill.description}</div>

          {generatedProperties.length > 0 ? (
            <div className="mt-3 grid gap-2 md:grid-cols-2">
              {generatedProperties.map(([key, property]) => {
                const type = property.type || "string";
                const value = parsedInputs[key];
                const description = typeof property.description === "string" ? property.description : "";
                if (type === "boolean") {
                  return (
                    <label key={key} className="flex items-center gap-2 rounded-md border border-white/10 bg-black/20 px-2 py-2">
                      <input
                        type="checkbox"
                        className="h-3.5 w-3.5"
                        checked={value === true}
                        onChange={(event) => updateInputsField(key, event.target.checked)}
                      />
                      <span className="flex-1 text-xs text-white/75">
                        {key}
                        {required.has(key) ? <span className="text-rose-200"> *</span> : null}
                        {description ? <span className="block text-[11px] text-white/45">{description}</span> : null}
                      </span>
                    </label>
                  );
                }
                return (
                  <label key={key} className="block rounded-md border border-white/10 bg-black/20 px-2 py-2 text-xs text-white/75">
                    <span>
                      {key}
                      {required.has(key) ? <span className="text-rose-200"> *</span> : null}
                    </span>
                    {description ? <span className="mt-0.5 block text-[11px] text-white/45">{description}</span> : null}
                    <input
                      type={type === "number" || type === "integer" ? "number" : "text"}
                      className="mt-1 w-full rounded border border-white/10 bg-black/40 px-2 py-1 text-[11px] text-white/85"
                      value={
                        type === "number" || type === "integer"
                          ? typeof value === "number"
                            ? String(value)
                            : ""
                          : typeof value === "string"
                            ? value
                            : ""
                      }
                      onChange={(event) =>
                        updateInputsField(
                          key,
                          event.target.value
                            ? coercePrimitiveInput(event.target.value, property)
                            : "",
                          { removeIfBlank: type === "string" || type === "number" || type === "integer" },
                        )
                      }
                    />
                  </label>
                );
              })}
            </div>
          ) : null}

          <div className="mt-3">
            <div className="mb-1 text-[11px] font-medium text-white/60">Inputs JSON</div>
            <textarea
              className="h-28 w-full rounded-md border border-white/10 bg-black/40 p-2 font-mono text-[11px] text-white/80"
              value={props.inputsJson}
              onChange={(event) => props.onSetInputsJson(event.target.value)}
              placeholder='{"goal":"Fix flaky test","test_command":"pytest -q"}'
            />
            {props.inputsParseError ? <div className="mt-1 text-xs text-rose-200">{props.inputsParseError}</div> : null}
          </div>
        </>
      ) : props.skills.length > 0 && !props.loading ? (
        <div className="mt-2 text-xs text-white/45">Select a workflow bundle to materialize it into the composer.</div>
      ) : null}
    </div>
  );
}
