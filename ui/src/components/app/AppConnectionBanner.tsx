import React from "react";

type AppConnectionBannerProps = {
  healthError: boolean;
  sessionsUnauthorized: boolean;
  connectionMode: "direct" | "broker";
  missingBrokerAuthToken: boolean;
  brokerCookieAuth: boolean;
  effectiveBase: string;
  webOrigin: string;
  missingDaemonAuthToken: boolean;
  isLocalDaemonBase: boolean;
  onShowSettings: () => void;
  onUseDevToken: () => void;
};

export default function AppConnectionBanner(props: AppConnectionBannerProps) {
  if (props.healthError) {
    return (
      <div className="border-b border-amber-500/20 bg-amber-500/10 px-4 py-2 text-xs text-amber-100">
        {props.connectionMode === "broker" && props.missingBrokerAuthToken ? (
          <>
            <span className="font-semibold text-amber-50/90">Unauthorized:</span> the broker requires an OIDC bearer token.
            Set it in{" "}
            <button className="underline hover:text-white" onClick={props.onShowSettings} type="button">
              Settings
            </button>{" "}
            (<span className="text-amber-50/90">Broker auth token</span>).
          </>
        ) : props.connectionMode === "broker" && props.brokerCookieAuth ? (
          <>
            <span className="font-semibold text-amber-50/90">Unauthorized:</span> broker cookie auth is enabled, but the browser did not send a valid broker auth cookie.
            Check the broker <code className="text-amber-50/90">--auth-cookie</code> / <code className="text-amber-50/90">--cors-allow-credentials</code> setup or disable cookie auth in{" "}
            <button className="underline hover:text-white" onClick={props.onShowSettings} type="button">
              Settings
            </button>
            .
          </>
        ) : (
          <>
            Browser cannot reach <code className="text-amber-50/90">{props.effectiveBase}</code> (network, TLS, or CORS).
            {props.webOrigin && props.connectionMode === "direct" ? (
              <>
                {" "}
                If <code className="text-amber-50/90">agentd</code> is running, allow this UI origin:{" "}
                <code className="text-amber-50/90">{props.webOrigin}</code> (start agentd with{" "}
                <code className="text-amber-50/90">--cors-origin {props.webOrigin}</code>).
              </>
            ) : null}
          </>
        )}
      </div>
    );
  }

  if (props.sessionsUnauthorized && props.missingDaemonAuthToken) {
    return (
      <div className="border-b border-amber-500/20 bg-amber-500/10 px-4 py-2 text-xs text-amber-100">
        <span className="font-semibold text-amber-50/90">Unauthorized:</span> the daemon requires a bearer token.
        Set it in <button className="underline hover:text-white" onClick={props.onShowSettings} type="button">Settings</button>{" "}
        (
        <span className="text-amber-50/90">
          {props.connectionMode === "broker" ? "Agentd auth token (X-Agentd-Authorization)" : "Daemon auth token"}
        </span>
        ).
        <span className="text-amber-50/80">
          {" "}
          If you started via docker-compose, it’s typically <code className="text-amber-50/90">dev-agentd-token</code>.
        </span>
        {props.isLocalDaemonBase ? (
          <>
            {" "}
            <button
              className="ml-2 rounded-md border border-amber-500/30 bg-amber-500/10 px-2 py-1 text-[11px] text-amber-50/90 hover:bg-amber-500/15"
              type="button"
              onClick={props.onUseDevToken}
              title="Convenience for local dev. For production, use a real bearer token."
            >
              Use dev token
            </button>
          </>
        ) : null}
      </div>
    );
  }

  return null;
}
