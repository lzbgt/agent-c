import React from "react";
import type { Diagnostics, DiagnosticsProviders } from "../../api/schemas/daemon";
import { SectionHeader } from "./SettingsControls";

function formatBytes(n?: number | null) {
  if (!n || !Number.isFinite(n) || n <= 0) return "";
  const units = ["B", "KB", "MB", "GB"];
  let idx = 0;
  let v = n;
  while (v >= 1024 && idx < units.length - 1) {
    v /= 1024;
    idx++;
  }
  const rounded = idx === 0 ? v.toFixed(0) : v.toFixed(2);
  return `${rounded} ${units[idx]}`;
}

function formatDuration(ms?: number | null) {
  if (!ms || !Number.isFinite(ms) || ms <= 0) return "";
  const s = Math.floor(ms / 1000);
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const rem = s % 60;
  if (h > 0) return `${h}h ${m}m ${rem}s`;
  if (m > 0) return `${m}m ${rem}s`;
  return `${rem}s`;
}

type SettingsDiagnosticsSectionProps = {
  diagnostics: Diagnostics | undefined;
  diagnosticsProviders: DiagnosticsProviders | undefined;
  diagnosticsFetching: boolean;
  diagnosticsProvidersFetching: boolean;
  diagnosticsError: string | null;
  diagnosticsProvidersError: string | null;
  onRefresh: () => void;
  sandboxMountHostPath: string;
  setSandboxMountHostPath: React.Dispatch<React.SetStateAction<string>>;
  sandboxMountContainerPath: string;
  setSandboxMountContainerPath: React.Dispatch<React.SetStateAction<string>>;
  sandboxMountContainerPrefix: string;
  setSandboxMountContainerPrefix: React.Dispatch<React.SetStateAction<string>>;
  sandboxMountIsMain: boolean;
  setSandboxMountIsMain: React.Dispatch<React.SetStateAction<boolean>>;
  sandboxMountPending: boolean;
  sandboxMountError: string | null;
  sandboxMountResult: unknown;
  onValidateSandboxMount: () => void;
  canValidateSandboxMount: boolean;
  providerTests: Record<string, any>;
  onRunProviderTest: (provider: string) => void;
};

export default function SettingsDiagnosticsSection(props: SettingsDiagnosticsSectionProps) {
  const {
    diagnostics,
    diagnosticsProviders,
    diagnosticsFetching,
    diagnosticsProvidersFetching,
    diagnosticsError,
    diagnosticsProvidersError,
    onRefresh,
    sandboxMountHostPath,
    setSandboxMountHostPath,
    sandboxMountContainerPath,
    setSandboxMountContainerPath,
    sandboxMountContainerPrefix,
    setSandboxMountContainerPrefix,
    sandboxMountIsMain,
    setSandboxMountIsMain,
    sandboxMountPending,
    sandboxMountError,
    sandboxMountResult,
    onValidateSandboxMount,
    canValidateSandboxMount,
    providerTests,
    onRunProviderTest,
  } = props;

  const providerEntries =
    diagnosticsProviders && diagnosticsProviders.providers && typeof diagnosticsProviders.providers === "object"
      ? (diagnosticsProviders.providers as Record<string, any>)
      : {};
  const deepseekKeyPresent = providerEntries?.deepseek?.key_present === true;
  const moonshotKeyPresent = providerEntries?.moonshot?.key_present === true;
  const glmKeyPresent = providerEntries?.glm?.key_present === true;
  const providerStatus = (name: string) => {
    const entry = providerTests[name];
    if (!entry) return null;
    if (entry.status === "running") return <span className="text-amber-200">running…</span>;
    if (entry.status === "ok") return <span className="text-emerald-200">ok</span>;
    if (entry.status === "error") return <span className="text-rose-200">error</span>;
    return null;
  };

  return (
    <div className="mt-4 rounded-md border border-white/10 bg-black/20 p-3">
      <SectionHeader
        title="Diagnostics"
        action={
          <button
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={diagnosticsFetching || diagnosticsProvidersFetching}
            onClick={onRefresh}
          >
            Refresh
          </button>
        }
      />
      <div className="mt-2 grid gap-2 text-[11px] text-white/70">
        <div>
          ready:{" "}
          <span className={diagnostics?.ready ? "text-emerald-200" : "text-rose-200"}>
            {typeof diagnostics?.ready === "boolean" ? String(diagnostics.ready) : "unknown"}
          </span>
          {diagnostics?.uptime_ms ? (
            <span className="text-white/50"> · uptime {formatDuration(diagnostics.uptime_ms)}</span>
          ) : null}
        </div>
        <div>
          db:{" "}
          <code className="text-white/70">
            {typeof diagnostics?.db?.path === "string" ? diagnostics.db.path : "(unknown)"}
          </code>
          {typeof diagnostics?.db?.size_bytes === "number" ? (
            <span className="text-white/50"> · {formatBytes(diagnostics.db.size_bytes)}</span>
          ) : null}
        </div>
        {diagnostics?.sandbox_mount_allowlist && typeof diagnostics.sandbox_mount_allowlist === "object" ? (
          <div>
            mount allowlist:{" "}
            <code className="text-white/70">
              {typeof (diagnostics.sandbox_mount_allowlist as any).path === "string"
                ? (diagnostics.sandbox_mount_allowlist as any).path
                : "(unknown)"}
            </code>
            <span className="text-white/50">
              {" "}
              · present {String((diagnostics.sandbox_mount_allowlist as any).present ?? false)} · loaded{" "}
              {String((diagnostics.sandbox_mount_allowlist as any).loaded ?? false)}
            </span>
            {typeof (diagnostics.sandbox_mount_allowlist as any).allowed_roots === "number" ? (
              <span className="text-white/50"> · roots {(diagnostics.sandbox_mount_allowlist as any).allowed_roots}</span>
            ) : null}
            {typeof (diagnostics.sandbox_mount_allowlist as any).blocked_patterns === "number" ? (
              <span className="text-white/50">
                {" "}
                · blocked {(diagnostics.sandbox_mount_allowlist as any).blocked_patterns}
              </span>
            ) : null}
            {typeof (diagnostics.sandbox_mount_allowlist as any).error === "string" &&
            (diagnostics.sandbox_mount_allowlist as any).error ? (
              <div className="text-rose-200">allowlist error: {(diagnostics.sandbox_mount_allowlist as any).error}</div>
            ) : null}
          </div>
        ) : null}
        <div className="rounded-md border border-white/10 bg-black/20 p-2">
          <div className="text-[11px] font-semibold text-white/60">Sandbox mount validator</div>
          <div className="mt-2 grid gap-2 text-[11px] text-white/70">
            <label className="grid gap-1">
              <span className="text-white/50">host_path</span>
              <input
                className="rounded border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80"
                value={String(sandboxMountHostPath || "")}
                onChange={(e) => setSandboxMountHostPath(e.target.value)}
                placeholder="~/Documents/project"
              />
            </label>
            <label className="grid gap-1">
              <span className="text-white/50">container_path</span>
              <input
                className="rounded border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80"
                value={String(sandboxMountContainerPath || "")}
                onChange={(e) => setSandboxMountContainerPath(e.target.value)}
                placeholder="/workspace/extra/project"
              />
            </label>
            <div className="flex flex-wrap items-center gap-3">
              <label className="flex items-center gap-2 text-[11px] text-white/60">
                container_prefix
                <input
                  className="w-[160px] rounded border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80"
                  value={String(sandboxMountContainerPrefix || "")}
                  onChange={(e) => setSandboxMountContainerPrefix(e.target.value)}
                />
              </label>
              <label className="flex items-center gap-2 text-[11px] text-white/60">
                <input
                  type="checkbox"
                  checked={!!sandboxMountIsMain}
                  onChange={(e) => setSandboxMountIsMain(e.target.checked)}
                />
                is_main
              </label>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                type="button"
                onClick={onValidateSandboxMount}
                disabled={!canValidateSandboxMount || sandboxMountPending}
              >
                {sandboxMountPending ? "Validating…" : "Validate"}
              </button>
            </div>
            {sandboxMountError ? <div className="text-[11px] text-rose-200">{sandboxMountError}</div> : null}
            {sandboxMountResult ? (
              <pre className="max-h-40 overflow-auto rounded border border-white/10 bg-black/30 p-2 text-[10px] text-white/70">
                {JSON.stringify(sandboxMountResult, null, 2)}
              </pre>
            ) : null}
          </div>
        </div>
        {diagnostics?.jobs && typeof diagnostics.jobs === "object" ? (
          <div>
            jobs total: <code className="text-white/70">{String((diagnostics.jobs as any).total ?? "(unknown)")}</code>
          </div>
        ) : null}
        {diagnostics?.workflows && typeof diagnostics.workflows === "object" ? (
          <div>
            workflows queued:{" "}
            <code className="text-white/70">{String((diagnostics.workflows as any).tasks_queued_ready ?? "(unknown)")}</code>
          </div>
        ) : null}
        {Array.isArray(diagnostics?.warnings) && diagnostics?.warnings.length > 0 ? (
          <div className="rounded-md border border-amber-500/30 bg-amber-500/10 px-2 py-1 text-amber-100">
            {diagnostics.warnings.join("; ")}
          </div>
        ) : null}
        {diagnosticsError ? (
          <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-rose-200">
            diagnostics failed: {diagnosticsError}
          </div>
        ) : null}
        {diagnosticsProvidersError ? (
          <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-rose-200">
            provider status failed: {diagnosticsProvidersError}
          </div>
        ) : null}
      </div>

      <div className="mt-3">
        <div className="text-[11px] font-semibold text-white/60">Providers</div>
        <div className="mt-2 rounded-md border border-white/10 bg-black/20">
          {["deepseek", "moonshot", "glm", "openrouter", "openai"].map((name) => {
            const provider = providerEntries[name] || {};
            const keyPresent = provider.key_present === true;
            const label =
              name === "moonshot" ? "Kimi (Moonshot CN)" : name === "glm" ? "GLM (Zhipu)" : name;
            const source =
              provider.source && typeof provider.source === "object"
                ? `${provider.source.kind ?? "source"}:${provider.source.label ?? "unknown"}`
                : "";
            const baseUrl = typeof provider.base_url === "string" ? provider.base_url : "";
            const model = typeof provider.model === "string" ? provider.model : "";
            const modelDefault = typeof provider.model_default === "string" ? provider.model_default : "";
            const warning = typeof provider.warning === "string" ? provider.warning : "";
            return (
              <div key={name} className="border-t border-white/5 px-3 py-2 text-[11px] text-white/70 first:border-t-0">
                <div className="flex items-center justify-between gap-2">
                  <span className="font-mono text-white/80">{label}</span>
                  <span className={keyPresent ? "text-emerald-200" : "text-rose-200"}>
                    key={keyPresent ? "present" : "missing"}
                  </span>
                </div>
                {source ? <div className="text-white/50">source: {source}</div> : null}
                {baseUrl ? <div className="text-white/50">base_url: {baseUrl}</div> : null}
                {model ? <div className="text-white/50">model: {model}</div> : null}
                {modelDefault ? <div className="text-white/40">default: {modelDefault}</div> : null}
                {warning ? <div className="text-amber-200/80">warning: {warning}</div> : null}
              </div>
            );
          })}
        </div>
      </div>

      <div className="mt-3 flex flex-wrap items-center gap-2 text-[11px] text-white/70">
        <button
          className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
          type="button"
          onClick={() => onRunProviderTest("deepseek")}
          disabled={!deepseekKeyPresent}
          title={deepseekKeyPresent ? "Run DeepSeek provider test" : "DeepSeek key missing"}
        >
          Test DeepSeek
        </button>
        <button
          className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
          type="button"
          onClick={() => onRunProviderTest("moonshot")}
          disabled={!moonshotKeyPresent}
          title={moonshotKeyPresent ? "Run Kimi (Moonshot) provider test" : "Kimi key missing"}
        >
          Test Kimi
        </button>
        <button
          className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
          type="button"
          onClick={() => onRunProviderTest("glm")}
          disabled={!glmKeyPresent}
          title={glmKeyPresent ? "Run GLM provider test" : "GLM key missing"}
        >
          Test GLM
        </button>
        <span>
          DeepSeek: {providerStatus("deepseek")} · Kimi: {providerStatus("moonshot")} · GLM: {providerStatus("glm")}
        </span>
      </div>
      {Object.entries(providerTests).map(([name, entry]) => {
        if (!entry || !entry.error) return null;
        return (
          <div
            key={`provider-error-${name}`}
            className="mt-2 rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200"
          >
            {name} test error: {String(entry.error)}
          </div>
        );
      })}
      <div className="mt-2 text-[11px] text-white/50">
        Provider tests use the diagnostics endpoint and will not persist keys in the browser.
      </div>
    </div>
  );
}
