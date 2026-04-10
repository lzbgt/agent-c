import React from "react";
import { useMutation } from "@tanstack/react-query";
import {
  apiAgentdTrace,
  apiBrokerTrace,
  type AgentdTraceResp,
  type ApiAuth,
  type BrokerTraceResp,
} from "../api";

export type TraceLookupArgs = {
  brokerBase: string;
  connectionMode: string;
  daemonAuth: ApiAuth;
  effectiveBase: string;
};

type TraceLookupResult =
  | { mode: "broker"; data: BrokerTraceResp }
  | { mode: "direct"; data: AgentdTraceResp };

export default function useTraceLookup(args: TraceLookupArgs) {
  const { brokerBase, connectionMode, daemonAuth, effectiveBase } = args;
  const [traceLookupError, setTraceLookupError] = React.useState<string | null>(null);
  const [traceLookupAgentd, setTraceLookupAgentd] = React.useState<AgentdTraceResp | null>(null);
  const [traceLookupBroker, setTraceLookupBroker] = React.useState<BrokerTraceResp | null>(null);

  const clearTraceLookup = React.useCallback(() => {
    setTraceLookupError(null);
    setTraceLookupAgentd(null);
    setTraceLookupBroker(null);
  }, []);

  const traceLookup = useMutation<TraceLookupResult, Error, string>({
    mutationFn: async (traceIdRaw: string) => {
      const traceId = String(traceIdRaw || "").trim();
      if (!traceId) throw new Error("missing trace_id");
      clearTraceLookup();

      if (connectionMode === "broker") {
        const base = String(brokerBase || "").trim();
        if (!base) throw new Error("missing broker base");
        return { mode: "broker" as const, data: await apiBrokerTrace(base, traceId, daemonAuth) };
      }
      return { mode: "direct" as const, data: await apiAgentdTrace(effectiveBase, traceId, daemonAuth) };
    },
    onSuccess: (value) => {
      if (value.mode === "broker") {
        setTraceLookupBroker(value.data);
      } else {
        setTraceLookupAgentd(value.data);
      }
    },
    onError: (error) => {
      setTraceLookupError(error instanceof Error ? error.message : String(error));
    },
  });

  return {
    clearTraceLookup,
    traceLookup,
    traceLookupAgentd,
    traceLookupBroker,
    traceLookupError,
  };
}
