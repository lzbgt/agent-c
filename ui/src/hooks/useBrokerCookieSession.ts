import React from "react";

import { apiBrokerCreateAuthSession, apiBrokerDeleteAuthSession } from "../api";
import type { ConnectionMode } from "../runtime_config";
import type { ConnectionSettings } from "./uiSettingsTypes";
import type { ConnectionProfile } from "./uiSettingsProfiles";
import type { ConnectionProfileUpdater } from "./useUiConnectionProfileState";

type UseBrokerCookieSessionParams = {
  activeProfileId: string;
  brokerAuthToken: string;
  brokerCookieAuth: boolean;
  connectionMode: ConnectionMode;
  serverPrefsBase: string;
  updateActiveProfile: ConnectionProfileUpdater;
};

type UseBrokerCookieSessionResult = {
  brokerCookieSessionStatus: ConnectionSettings["brokerCookieSessionStatus"];
  brokerCookieSessionError: string | null;
  clearBrokerAuthCookie: () => Promise<void>;
};

export default function useBrokerCookieSession({
  activeProfileId,
  brokerAuthToken,
  brokerCookieAuth,
  connectionMode,
  serverPrefsBase,
  updateActiveProfile,
}: UseBrokerCookieSessionParams): UseBrokerCookieSessionResult {
  const [brokerCookieSessionStatus, setBrokerCookieSessionStatus] = React.useState<ConnectionSettings["brokerCookieSessionStatus"]>(
    "idle",
  );
  const [brokerCookieSessionError, setBrokerCookieSessionError] = React.useState<string | null>(null);
  const brokerCookieExchangeKeyRef = React.useRef("");

  React.useEffect(() => {
    if (connectionMode !== "broker" || !brokerCookieAuth) {
      brokerCookieExchangeKeyRef.current = "";
      setBrokerCookieSessionStatus("idle");
      setBrokerCookieSessionError(null);
      return;
    }
    const baseTrimmed = String(serverPrefsBase || "").trim();
    const token = String(brokerAuthToken || "").trim();
    if (!baseTrimmed) {
      brokerCookieExchangeKeyRef.current = "";
      setBrokerCookieSessionStatus("idle");
      setBrokerCookieSessionError(null);
      return;
    }
    if (!token) {
      brokerCookieExchangeKeyRef.current = "";
      setBrokerCookieSessionError(null);
      setBrokerCookieSessionStatus((prev) => (prev === "ready" ? prev : "idle"));
      return;
    }
    const exchangeKey = `${activeProfileId}:${token}`;
    if (brokerCookieExchangeKeyRef.current === exchangeKey) return;
    brokerCookieExchangeKeyRef.current = exchangeKey;
    setBrokerCookieSessionStatus("exchanging");
    setBrokerCookieSessionError(null);
    (async () => {
      try {
        await apiBrokerCreateAuthSession(baseTrimmed, { mode: "broker", token });
        updateActiveProfile((prev: ConnectionProfile) => {
          if (prev.mode !== "broker" || !prev.brokerCookieAuth) return prev;
          if (String(prev.brokerAuthToken || "").trim() !== token) return prev;
          return { ...prev, brokerAuthToken: "" };
        });
        if (typeof window !== "undefined") {
          try {
            window.sessionStorage.removeItem("agentui.brokerAuthToken");
          } catch {
            // ignore storage failures
          }
        }
        if (brokerCookieExchangeKeyRef.current === exchangeKey) {
          brokerCookieExchangeKeyRef.current = "";
        }
        setBrokerCookieSessionStatus("ready");
        setBrokerCookieSessionError(null);
      } catch (err) {
        if (brokerCookieExchangeKeyRef.current === exchangeKey) {
          brokerCookieExchangeKeyRef.current = "";
        }
        setBrokerCookieSessionStatus("error");
        setBrokerCookieSessionError(err instanceof Error ? err.message : String(err));
      }
    })();
  }, [activeProfileId, brokerAuthToken, brokerCookieAuth, connectionMode, serverPrefsBase, updateActiveProfile]);

  const clearBrokerAuthCookie = React.useCallback(async () => {
    const baseTrimmed = String(serverPrefsBase || "").trim();
    if (connectionMode !== "broker" || !brokerCookieAuth || !baseTrimmed) {
      return;
    }
    setBrokerCookieSessionError(null);
    await apiBrokerDeleteAuthSession(baseTrimmed, { mode: "broker", useCookieAuth: true });
    setBrokerCookieSessionStatus("idle");
  }, [brokerCookieAuth, connectionMode, serverPrefsBase]);

  return {
    brokerCookieSessionStatus,
    brokerCookieSessionError,
    clearBrokerAuthCookie,
  };
}
