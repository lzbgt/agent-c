import React from "react";
import { useMutation, useQuery } from "@tanstack/react-query";
import type { ApiAuth } from "../../api";
import {
  apiAttachSessionService,
  apiGetSessionCapability,
  apiGetSessionOrchestrationDependencies,
  apiGetSessionOrchestrationStatus,
  apiGetSessionOrchestrationWorkers,
  apiGetSessionService,
  apiGetSessionShell,
  apiListSessionCapabilities,
  apiListSessionServices,
  apiListSessionShells,
  apiPollSessionShell,
  apiRunSessionService,
  apiSendSessionShell,
  apiStartSessionShell,
  apiTerminateSessionShell,
  apiWaitSessionService,
  extractSessionErrorMessage,
} from "../../api";
import type { ClientSettings, ConnectionSettings } from "../../hooks/useUiSettings";
import FieldLabel from "../FieldLabel";

type SessionSettings = {
  id: string;
  leaseSeconds: string;
  info?: {
    attachment?: {
      client_id?: string | null;
      lease_active?: boolean;
    };
  };
};

type SettingsBrokerSessionOperatorsSectionProps = {
  connection: ConnectionSettings;
  client: ClientSettings;
  session: SessionSettings;
};

function jsonText(value: unknown): string {
  try {
    return JSON.stringify(value ?? {}, null, 2);
  } catch {
    return String(value ?? "");
  }
}

function parsePositiveInt(value: string): number | undefined {
  const n = Number(value);
  return Number.isFinite(n) && n > 0 ? Math.floor(n) : undefined;
}

function readRows(payload: any, key: string): any[] {
  return payload?.ok && Array.isArray(payload?.[key]) ? payload[key] : [];
}

function shellRefOf(row: any): string {
  return String(row?.job_id || row?.job_ref || row?.alias || row?.id || "").trim();
}

function serviceRefOf(row: any): string {
  return String(row?.job_id || row?.job_ref || row?.alias || row?.id || "").trim();
}

function capabilityRefOf(row: any): string {
  return String(row?.name || row?.capability || row?.id || "").trim();
}

function shellSummary(row: any): string {
  const ref = shellRefOf(row) || "shell";
  const label = String(row?.label || row?.intent || "").trim();
  const status = String(row?.status || row?.state || "").trim();
  return [ref, label, status].filter(Boolean).join(" · ");
}

function serviceSummary(row: any): string {
  const ref = serviceRefOf(row) || "service";
  const label = String(row?.label || "").trim();
  const status = String(row?.status || row?.state || "").trim();
  return [ref, label, status].filter(Boolean).join(" · ");
}

function capabilitySummary(row: any): string {
  const ref = capabilityRefOf(row) || "capability";
  const providers = Array.isArray(row?.providers) ? row.providers.length : 0;
  const consumers = Array.isArray(row?.consumers) ? row.consumers.length : 0;
  return `${ref}${providers || consumers ? ` · providers=${providers} consumers=${consumers}` : ""}`;
}

function shellOutputText(payload: any): string {
  const candidates = [
    payload?.stdout,
    payload?.stderr,
    payload?.job?.stdout,
    payload?.job?.stderr,
    payload?.output,
    payload?.job?.output,
  ];
  for (const value of candidates) {
    if (typeof value === "string" && value.trim().length > 0) return value;
  }
  return "";
}

export default function SettingsBrokerSessionOperatorsSection(props: SettingsBrokerSessionOperatorsSectionProps) {
  const { connection, client, session } = props;
  const sessionId = String(session.id || "").trim();
  const enabled = connection.mode === "broker" && sessionId.length > 0 && String(connection.effectiveBase || "").trim().length > 0;
  const daemonAuth: ApiAuth = connection.daemonAuth;
  const currentClientId = String(client.clientId || "").trim();
  const holderClientId = String(session.info?.attachment?.client_id || "").trim();
  const leaseRole =
    session.info?.attachment?.lease_active === true
      ? holderClientId && currentClientId && holderClientId === currentClientId
        ? "owner"
        : "observer"
      : "unleased";
  const mutationOpts = React.useMemo(
    () => ({
      clientId: currentClientId || undefined,
      leaseSeconds: parsePositiveInt(session.leaseSeconds) ?? 90,
    }),
    [currentClientId, session.leaseSeconds],
  );

  const [shellCommand, setShellCommand] = React.useState("pwd");
  const [shellIntent, setShellIntent] = React.useState("observation");
  const [shellLabel, setShellLabel] = React.useState("");
  const [selectedShellRef, setSelectedShellRef] = React.useState("");
  const [shellInput, setShellInput] = React.useState("");
  const [shellNotice, setShellNotice] = React.useState<string | null>(null);

  const [selectedServiceRef, setSelectedServiceRef] = React.useState("");
  const [serviceWaitMs, setServiceWaitMs] = React.useState("5000");
  const [serviceRecipe, setServiceRecipe] = React.useState("health");
  const [serviceArgsJson, setServiceArgsJson] = React.useState("{}");
  const [serviceNotice, setServiceNotice] = React.useState<string | null>(null);

  const [selectedCapabilityRef, setSelectedCapabilityRef] = React.useState("");

  const orchestrationStatus = useQuery({
    queryKey: ["session_orchestration_status", connection.effectiveBase, connection.authKey, sessionId],
    enabled,
    queryFn: () => apiGetSessionOrchestrationStatus(connection.effectiveBase, sessionId, daemonAuth),
    retry: 1,
  });
  const orchestrationWorkers = useQuery({
    queryKey: ["session_orchestration_workers", connection.effectiveBase, connection.authKey, sessionId],
    enabled,
    queryFn: () => apiGetSessionOrchestrationWorkers(connection.effectiveBase, sessionId, daemonAuth),
    retry: 1,
  });
  const orchestrationDependencies = useQuery({
    queryKey: ["session_orchestration_dependencies", connection.effectiveBase, connection.authKey, sessionId],
    enabled,
    queryFn: () => apiGetSessionOrchestrationDependencies(connection.effectiveBase, sessionId, daemonAuth),
    retry: 1,
  });
  const shells = useQuery({
    queryKey: ["session_shells", connection.effectiveBase, connection.authKey, sessionId],
    enabled,
    queryFn: () => apiListSessionShells(connection.effectiveBase, sessionId, daemonAuth),
    retry: 1,
  });
  const services = useQuery({
    queryKey: ["session_services", connection.effectiveBase, connection.authKey, sessionId],
    enabled,
    queryFn: () => apiListSessionServices(connection.effectiveBase, sessionId, daemonAuth),
    retry: 1,
  });
  const capabilities = useQuery({
    queryKey: ["session_capabilities", connection.effectiveBase, connection.authKey, sessionId],
    enabled,
    queryFn: () => apiListSessionCapabilities(connection.effectiveBase, sessionId, daemonAuth),
    retry: 1,
  });

  const shellRows = React.useMemo(() => readRows(shells.data, "shells"), [shells.data]);
  const serviceRows = React.useMemo(() => readRows(services.data, "services"), [services.data]);
  const capabilityRows = React.useMemo(() => readRows(capabilities.data, "capabilities"), [capabilities.data]);

  React.useEffect(() => {
    if (selectedShellRef) return;
    const fallback = shellRefOf(shellRows[0]);
    if (fallback) setSelectedShellRef(fallback);
  }, [selectedShellRef, shellRows]);

  React.useEffect(() => {
    if (selectedServiceRef) return;
    const fallback = serviceRefOf(serviceRows[0]);
    if (fallback) setSelectedServiceRef(fallback);
  }, [selectedServiceRef, serviceRows]);

  React.useEffect(() => {
    if (selectedCapabilityRef) return;
    const fallback = capabilityRefOf(capabilityRows[0]);
    if (fallback) setSelectedCapabilityRef(fallback);
  }, [capabilityRows, selectedCapabilityRef]);

  const shellDetail = useQuery({
    queryKey: ["session_shell_detail", connection.effectiveBase, connection.authKey, sessionId, selectedShellRef],
    enabled: enabled && selectedShellRef.length > 0,
    queryFn: () => apiGetSessionShell(connection.effectiveBase, sessionId, selectedShellRef, daemonAuth),
    retry: 1,
  });
  const serviceDetail = useQuery({
    queryKey: ["session_service_detail", connection.effectiveBase, connection.authKey, sessionId, selectedServiceRef],
    enabled: enabled && selectedServiceRef.length > 0,
    queryFn: () => apiGetSessionService(connection.effectiveBase, sessionId, selectedServiceRef, daemonAuth),
    retry: 1,
  });
  const capabilityDetail = useQuery({
    queryKey: ["session_capability_detail", connection.effectiveBase, connection.authKey, sessionId, selectedCapabilityRef],
    enabled: enabled && selectedCapabilityRef.length > 0,
    queryFn: () => apiGetSessionCapability(connection.effectiveBase, sessionId, selectedCapabilityRef, daemonAuth),
    retry: 1,
  });

  const refreshOperatorState = React.useCallback(() => {
    void orchestrationStatus.refetch();
    void orchestrationWorkers.refetch();
    void orchestrationDependencies.refetch();
    void shells.refetch();
    void services.refetch();
    void capabilities.refetch();
    if (selectedShellRef) void shellDetail.refetch();
    if (selectedServiceRef) void serviceDetail.refetch();
    if (selectedCapabilityRef) void capabilityDetail.refetch();
  }, [
    capabilities,
    capabilityDetail,
    orchestrationDependencies,
    orchestrationStatus,
    orchestrationWorkers,
    selectedCapabilityRef,
    selectedServiceRef,
    selectedShellRef,
    serviceDetail,
    services,
    shellDetail,
    shells,
  ]);

  const startShell = useMutation({
    mutationFn: async () => {
      const command = String(shellCommand || "").trim();
      if (!command) throw new Error("missing shell command");
      return apiStartSessionShell(
        connection.effectiveBase,
        sessionId,
        {
          command,
          intent: String(shellIntent || "").trim() || undefined,
          label: String(shellLabel || "").trim() || undefined,
        },
        daemonAuth,
        mutationOpts,
      );
    },
    onSuccess: async (resp) => {
      setShellNotice(jsonText(resp));
      await shells.refetch();
    },
  });

  const pollShell = useMutation({
    mutationFn: async () => {
      if (!selectedShellRef) throw new Error("missing shell ref");
      return apiPollSessionShell(connection.effectiveBase, sessionId, selectedShellRef, daemonAuth, mutationOpts);
    },
    onSuccess: async (resp) => {
      setShellNotice(jsonText(resp));
      await shellDetail.refetch();
      await shells.refetch();
    },
  });

  const sendShell = useMutation({
    mutationFn: async () => {
      if (!selectedShellRef) throw new Error("missing shell ref");
      return apiSendSessionShell(connection.effectiveBase, sessionId, selectedShellRef, shellInput, daemonAuth, mutationOpts);
    },
    onSuccess: async (resp) => {
      setShellNotice(jsonText(resp));
      setShellInput("");
      await shellDetail.refetch();
    },
  });

  const terminateShell = useMutation({
    mutationFn: async () => {
      if (!selectedShellRef) throw new Error("missing shell ref");
      return apiTerminateSessionShell(connection.effectiveBase, sessionId, selectedShellRef, daemonAuth, mutationOpts);
    },
    onSuccess: async (resp) => {
      setShellNotice(jsonText(resp));
      await shellDetail.refetch();
      await shells.refetch();
    },
  });

  const attachService = useMutation({
    mutationFn: async () => {
      if (!selectedServiceRef) throw new Error("missing service ref");
      return apiAttachSessionService(connection.effectiveBase, sessionId, selectedServiceRef, daemonAuth, mutationOpts);
    },
    onSuccess: async (resp) => {
      setServiceNotice(jsonText(resp));
      await serviceDetail.refetch();
      await services.refetch();
    },
  });

  const waitService = useMutation({
    mutationFn: async () => {
      if (!selectedServiceRef) throw new Error("missing service ref");
      return apiWaitSessionService(
        connection.effectiveBase,
        sessionId,
        selectedServiceRef,
        daemonAuth,
        { timeout_ms: parsePositiveInt(serviceWaitMs) },
        mutationOpts,
      );
    },
    onSuccess: async (resp) => {
      setServiceNotice(jsonText(resp));
      await serviceDetail.refetch();
      await services.refetch();
    },
  });

  const runService = useMutation({
    mutationFn: async () => {
      if (!selectedServiceRef) throw new Error("missing service ref");
      const recipe = String(serviceRecipe || "").trim();
      if (!recipe) throw new Error("missing service recipe");
      let args: Record<string, unknown> | undefined;
      const raw = String(serviceArgsJson || "").trim();
      if (raw && raw !== "{}") {
        const parsed = JSON.parse(raw);
        if (parsed && typeof parsed === "object" && !Array.isArray(parsed)) {
          args = parsed as Record<string, unknown>;
        } else {
          throw new Error("service args JSON must decode to an object");
        }
      }
      return apiRunSessionService(connection.effectiveBase, sessionId, selectedServiceRef, { recipe, args }, daemonAuth, mutationOpts);
    },
    onSuccess: async (resp) => {
      setServiceNotice(jsonText(resp));
      await serviceDetail.refetch();
    },
  });

  if (connection.mode !== "broker") return null;

  return (
    <div className="mt-4 rounded-md border border-white/10 bg-black/10 p-3" data-testid="broker-session-operators-section">
      <div className="flex items-center justify-between gap-2">
        <div>
          <FieldLabel>Broker session operators</FieldLabel>
          <div className="mt-1 text-[11px] text-white/60">
            Shell-first host examination for the current broker session. Artifact browsing is still intentionally transcript/event/shell based.
          </div>
        </div>
        <button
          className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
          type="button"
          onClick={refreshOperatorState}
          disabled={!enabled}
        >
          Refresh all
        </button>
      </div>

      {!sessionId ? <div className="mt-3 text-[11px] text-white/60">Select or create a session first.</div> : null}
      {sessionId ? (
        <div className="mt-2 text-[11px] text-white/60">
          lease role: <span className="font-mono text-white/80">{leaseRole}</span>
          {leaseRole !== "owner" ? " · lease-owned shell/service mutations may return attachment_conflict." : ""}
        </div>
      ) : null}

      {enabled ? (
        <>
          <div className="mt-4 rounded-md border border-white/10 bg-black/20 p-3">
            <div className="flex items-center justify-between gap-2">
              <div className="text-xs font-semibold text-white/80">Orchestration</div>
              <div className="text-[11px] text-white/50">
                status {orchestrationStatus.isFetching ? "loading" : "ready"} · workers {orchestrationWorkers.isFetching ? "loading" : "ready"} · dependencies{" "}
                {orchestrationDependencies.isFetching ? "loading" : "ready"}
              </div>
            </div>
            <div className="mt-2 grid grid-cols-1 gap-3">
              <div>
                <div className="text-[11px] text-white/60">Status</div>
                <pre className="mt-1 max-h-32 overflow-auto rounded-md border border-white/10 bg-black/30 p-2 text-[11px] text-white/80">{jsonText(orchestrationStatus.data || {})}</pre>
              </div>
              <div>
                <div className="text-[11px] text-white/60">Workers</div>
                <pre className="mt-1 max-h-32 overflow-auto rounded-md border border-white/10 bg-black/30 p-2 text-[11px] text-white/80">{jsonText(orchestrationWorkers.data || {})}</pre>
              </div>
              <div>
                <div className="text-[11px] text-white/60">Dependencies</div>
                <pre className="mt-1 max-h-32 overflow-auto rounded-md border border-white/10 bg-black/30 p-2 text-[11px] text-white/80">{jsonText(orchestrationDependencies.data || {})}</pre>
              </div>
            </div>
          </div>

          <div className="mt-4 rounded-md border border-white/10 bg-black/20 p-3" data-testid="session-shells-section">
            <div className="text-xs font-semibold text-white/80">Shells</div>
            <div className="mt-1 text-[11px] text-white/60">Use shells for remote host examination. Keep transcript and live session events alongside shell output for correlation.</div>
            <div className="mt-3 grid grid-cols-1 gap-3">
              <div>
                <FieldLabel>Start shell</FieldLabel>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                  data-testid="session-shell-command-input"
                  value={shellCommand}
                  onChange={(e) => setShellCommand(e.target.value)}
                  placeholder="pwd"
                />
                <div className="mt-2 grid grid-cols-2 gap-2">
                  <select
                    className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                    value={shellIntent}
                    onChange={(e) => setShellIntent(e.target.value)}
                  >
                    <option value="observation">observation</option>
                    <option value="prerequisite">prerequisite</option>
                    <option value="service">service</option>
                  </select>
                  <input
                    className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                    value={shellLabel}
                    onChange={(e) => setShellLabel(e.target.value)}
                    placeholder="optional label"
                  />
                </div>
                <button
                  className="mt-2 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                  type="button"
                  onClick={() => void startShell.mutateAsync().catch(() => {})}
                  disabled={startShell.isPending || !shellCommand.trim()}
                >
                  {startShell.isPending ? "Starting…" : "Start shell"}
                </button>
                {startShell.isError ? <div className="mt-1 text-[11px] text-rose-200">{String(startShell.error)}</div> : null}
              </div>
              <div className="grid grid-cols-2 gap-3">
                <div>
                  <div className="text-[11px] text-white/60">Active shells</div>
                  <div className="mt-1 max-h-48 overflow-auto rounded-md border border-white/10 bg-black/30">
                    {shellRows.length === 0 ? <div className="px-3 py-2 text-[11px] text-white/50">No remote shell jobs reported.</div> : null}
                    {shellRows.map((row, idx) => {
                      const ref = shellRefOf(row) || `shell-${idx}`;
                      return (
                        <button
                          key={`${ref}-${idx}`}
                          type="button"
                          className={`block w-full px-3 py-2 text-left text-[11px] hover:bg-white/5 ${selectedShellRef === ref ? "bg-white/10" : ""}`}
                          onClick={() => setSelectedShellRef(ref)}
                        >
                          {shellSummary(row)}
                        </button>
                      );
                    })}
                  </div>
                </div>
                <div>
                  <div className="flex items-center justify-between gap-2">
                    <div className="text-[11px] text-white/60">Shell detail</div>
                    <div className="flex items-center gap-2">
                      <button
                        className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                        type="button"
                        onClick={() => void pollShell.mutateAsync().catch(() => {})}
                        disabled={!selectedShellRef || pollShell.isPending}
                      >
                        Poll
                      </button>
                      <button
                        className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                        type="button"
                        onClick={() => void terminateShell.mutateAsync().catch(() => {})}
                        disabled={!selectedShellRef || terminateShell.isPending}
                      >
                        Terminate
                      </button>
                    </div>
                  </div>
                  <pre className="mt-1 max-h-48 overflow-auto rounded-md border border-white/10 bg-black/30 p-2 text-[11px] text-white/80">{jsonText(shellDetail.data || {})}</pre>
                  {shellOutputText(shellDetail.data) ? (
                    <>
                      <div className="mt-2 text-[11px] text-white/60">Output</div>
                      <pre className="mt-1 max-h-40 overflow-auto rounded-md border border-white/10 bg-black/30 p-2 text-[11px] text-white/80">{shellOutputText(shellDetail.data)}</pre>
                    </>
                  ) : null}
                  <div className="mt-2 flex gap-2">
                    <input
                      className="min-w-0 flex-1 rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                      value={shellInput}
                      onChange={(e) => setShellInput(e.target.value)}
                      placeholder="stdin text"
                    />
                    <button
                      className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                      type="button"
                      onClick={() => void sendShell.mutateAsync().catch(() => {})}
                      disabled={!selectedShellRef || !shellInput.trim() || sendShell.isPending}
                    >
                      Send
                    </button>
                  </div>
                  {shellNotice ? <pre className="mt-2 max-h-28 overflow-auto rounded-md border border-white/10 bg-black/30 p-2 text-[11px] text-white/70">{shellNotice}</pre> : null}
                  {pollShell.isError ? <div className="mt-1 text-[11px] text-rose-200">{String(pollShell.error)}</div> : null}
                  {sendShell.isError ? <div className="mt-1 text-[11px] text-rose-200">{String(sendShell.error)}</div> : null}
                  {terminateShell.isError ? <div className="mt-1 text-[11px] text-rose-200">{String(terminateShell.error)}</div> : null}
                </div>
              </div>
            </div>
          </div>

          <div className="mt-4 rounded-md border border-white/10 bg-black/20 p-3" data-testid="session-services-section">
            <div className="text-xs font-semibold text-white/80">Services</div>
            <div className="mt-1 text-[11px] text-white/60">Service inspection and recipe execution stay scoped to the current session.</div>
            <div className="mt-3 grid grid-cols-2 gap-3">
              <div>
                <div className="text-[11px] text-white/60">Available services</div>
                <div className="mt-1 max-h-48 overflow-auto rounded-md border border-white/10 bg-black/30">
                  {serviceRows.length === 0 ? <div className="px-3 py-2 text-[11px] text-white/50">No remote services reported.</div> : null}
                  {serviceRows.map((row, idx) => {
                    const ref = serviceRefOf(row) || `service-${idx}`;
                    return (
                      <button
                        key={`${ref}-${idx}`}
                        type="button"
                        className={`block w-full px-3 py-2 text-left text-[11px] hover:bg-white/5 ${selectedServiceRef === ref ? "bg-white/10" : ""}`}
                        onClick={() => setSelectedServiceRef(ref)}
                      >
                        {serviceSummary(row)}
                      </button>
                    );
                  })}
                </div>
              </div>
              <div>
                <div className="flex items-center justify-between gap-2">
                  <div className="text-[11px] text-white/60">Service detail</div>
                  <div className="flex items-center gap-2">
                    <button
                      className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                      type="button"
                      onClick={() => void attachService.mutateAsync().catch(() => {})}
                      disabled={!selectedServiceRef || attachService.isPending}
                    >
                      Attach
                    </button>
                    <button
                      className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                      type="button"
                      onClick={() => void waitService.mutateAsync().catch(() => {})}
                      disabled={!selectedServiceRef || waitService.isPending}
                    >
                      Wait
                    </button>
                  </div>
                </div>
                <pre className="mt-1 max-h-48 overflow-auto rounded-md border border-white/10 bg-black/30 p-2 text-[11px] text-white/80">{jsonText(serviceDetail.data || {})}</pre>
                <div className="mt-2 grid grid-cols-[120px,1fr] gap-2">
                  <input
                    className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                    value={serviceWaitMs}
                    onChange={(e) => setServiceWaitMs(e.target.value)}
                    placeholder="timeout ms"
                  />
                  <input
                    className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                    value={serviceRecipe}
                    onChange={(e) => setServiceRecipe(e.target.value)}
                    placeholder="recipe"
                  />
                </div>
                <textarea
                  className="mt-2 min-h-[88px] w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                  value={serviceArgsJson}
                  onChange={(e) => setServiceArgsJson(e.target.value)}
                  placeholder='{"path":"/health"}'
                />
                <button
                  className="mt-2 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                  type="button"
                  onClick={() => void runService.mutateAsync().catch(() => {})}
                  disabled={!selectedServiceRef || !serviceRecipe.trim() || runService.isPending}
                >
                  Run recipe
                </button>
                {serviceNotice ? <pre className="mt-2 max-h-28 overflow-auto rounded-md border border-white/10 bg-black/30 p-2 text-[11px] text-white/70">{serviceNotice}</pre> : null}
                {attachService.isError ? <div className="mt-1 text-[11px] text-rose-200">{String(attachService.error)}</div> : null}
                {waitService.isError ? <div className="mt-1 text-[11px] text-rose-200">{String(waitService.error)}</div> : null}
                {runService.isError ? <div className="mt-1 text-[11px] text-rose-200">{String(runService.error)}</div> : null}
              </div>
            </div>
          </div>

          <div className="mt-4 rounded-md border border-white/10 bg-black/20 p-3" data-testid="session-capabilities-section">
            <div className="text-xs font-semibold text-white/80">Capabilities</div>
            <div className="mt-1 text-[11px] text-white/60">Use capabilities to explain what the remote session can do before inventing new synthetic controls.</div>
            <div className="mt-3 grid grid-cols-2 gap-3">
              <div>
                <div className="text-[11px] text-white/60">Available capabilities</div>
                <div className="mt-1 max-h-40 overflow-auto rounded-md border border-white/10 bg-black/30">
                  {capabilityRows.length === 0 ? <div className="px-3 py-2 text-[11px] text-white/50">No capabilities reported.</div> : null}
                  {capabilityRows.map((row, idx) => {
                    const ref = capabilityRefOf(row) || `cap-${idx}`;
                    return (
                      <button
                        key={`${ref}-${idx}`}
                        type="button"
                        className={`block w-full px-3 py-2 text-left text-[11px] hover:bg-white/5 ${selectedCapabilityRef === ref ? "bg-white/10" : ""}`}
                        onClick={() => setSelectedCapabilityRef(ref)}
                      >
                        {capabilitySummary(row)}
                      </button>
                    );
                  })}
                </div>
              </div>
              <div>
                <div className="text-[11px] text-white/60">Capability detail</div>
                <pre className="mt-1 max-h-40 overflow-auto rounded-md border border-white/10 bg-black/30 p-2 text-[11px] text-white/80">{jsonText(capabilityDetail.data || {})}</pre>
              </div>
            </div>
          </div>

          {extractSessionErrorMessage(shells.data) || extractSessionErrorMessage(services.data) || extractSessionErrorMessage(capabilities.data) ? (
            <div className="mt-3 text-[11px] text-rose-200">
              operator read error: {extractSessionErrorMessage(shells.data) || extractSessionErrorMessage(services.data) || extractSessionErrorMessage(capabilities.data)}
            </div>
          ) : null}
        </>
      ) : null}
    </div>
  );
}
