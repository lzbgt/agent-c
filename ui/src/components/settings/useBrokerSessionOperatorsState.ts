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
import type { SettingsBrokerSessionOperatorsSectionProps } from "./settingsBrokerSessionOperatorTypes";
import {
  capabilityRefOf,
  jsonText,
  parsePositiveInt,
  readRows,
  serviceRefOf,
  shellRefOf,
} from "./settingsBrokerSessionOperatorUtils";

export default function useBrokerSessionOperatorsState(props: SettingsBrokerSessionOperatorsSectionProps) {
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
    if (selectedShellRef && shellRows.some((row) => shellRefOf(row) === selectedShellRef)) return;
    const fallback = shellRefOf(shellRows[0]);
    setSelectedShellRef(fallback || "");
  }, [selectedShellRef, shellRows]);

  React.useEffect(() => {
    if (selectedServiceRef && serviceRows.some((row) => serviceRefOf(row) === selectedServiceRef)) return;
    const fallback = serviceRefOf(serviceRows[0]);
    setSelectedServiceRef(fallback || "");
  }, [selectedServiceRef, serviceRows]);

  React.useEffect(() => {
    if (selectedCapabilityRef && capabilityRows.some((row) => capabilityRefOf(row) === selectedCapabilityRef)) return;
    const fallback = capabilityRefOf(capabilityRows[0]);
    setSelectedCapabilityRef(fallback || "");
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

  const operatorReadError =
    extractSessionErrorMessage(shells.data) || extractSessionErrorMessage(services.data) || extractSessionErrorMessage(capabilities.data);

  return {
    enabled,
    leaseRole,
    sessionId,
    shellCommand,
    setShellCommand,
    shellIntent,
    setShellIntent,
    shellLabel,
    setShellLabel,
    selectedShellRef,
    setSelectedShellRef,
    shellInput,
    setShellInput,
    shellNotice,
    selectedServiceRef,
    setSelectedServiceRef,
    serviceWaitMs,
    setServiceWaitMs,
    serviceRecipe,
    setServiceRecipe,
    serviceArgsJson,
    setServiceArgsJson,
    serviceNotice,
    selectedCapabilityRef,
    setSelectedCapabilityRef,
    orchestrationStatus,
    orchestrationWorkers,
    orchestrationDependencies,
    shells,
    services,
    capabilities,
    shellRows,
    serviceRows,
    capabilityRows,
    shellDetail,
    serviceDetail,
    capabilityDetail,
    refreshOperatorState,
    startShell,
    pollShell,
    sendShell,
    terminateShell,
    attachService,
    waitService,
    runService,
    operatorReadError,
  };
}
