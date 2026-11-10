import React from "react";
import { useQuery, useQueryClient } from "@tanstack/react-query";

type Identity = { displayName: string; signOut: () => Promise<void>; signingOut: boolean };
const IdentityContext = React.createContext<Identity | null>(null);
export const useDeploymentIdentity = () => React.useContext(IdentityContext);

async function loadSession() {
  const response = await fetch("/auth/session", { credentials: "same-origin", signal: AbortSignal.timeout(12000), cache: "no-store" });
  if (!response.ok) throw new Error("Account verification is temporarily unavailable. Please retry.");
  const value = await response.json();
  return { authenticated: value.authenticated === true, admin: value.admin === true, displayName: typeof value.display_name === "string" ? value.display_name : "Administrator" };
}

function SessionGate({ children }: { children: React.ReactNode }) {
  const cache = useQueryClient();
  const session = useQuery({ queryKey: ["deploymentSession"], queryFn: loadSession, retry: false, staleTime: 10000, refetchInterval: 30000 });
  const [signingOut, setSigningOut] = React.useState(false);
  const [logoutError, setLogoutError] = React.useState("");
  const providers = useQuery({
    queryKey: ["deploymentProviders"], enabled: session.data?.authenticated === false,
    queryFn: async () => {
      const response = await fetch("/auth/providers", { credentials: "same-origin", signal: AbortSignal.timeout(12000) });
      if (!response.ok) throw new Error("Sign-in is temporarily unavailable. Please retry.");
      return await response.json() as Record<string, boolean>;
    }, retry: false,
  });
  async function signOut() {
    setSigningOut(true); setLogoutError("");
    try {
      const response = await fetch("/auth/logout", { method: "POST", credentials: "same-origin" });
      if (!response.ok) throw new Error("Could not sign out. Please retry.");
      // Clear conversation/query data before another account opens the workspace.
      cache.clear();
      window.location.assign("/");
    } catch (error) { setLogoutError(String(error)); setSigningOut(false); }
  }
  if (session.data?.authenticated && session.data.admin && !session.isError) {
    return <IdentityContext.Provider value={{ displayName: session.data.displayName, signOut, signingOut }}>
      {children}
      {logoutError ? <div role="alert" className="fixed bottom-4 right-4 z-50 rounded-lg bg-rose-950 px-4 py-3 text-sm text-rose-100">{logoutError}</div> : null}
    </IdentityContext.Provider>;
  }
  const waiting = session.isPending;
  const member = session.data?.authenticated && !session.data.admin;
  return <main className="identity-page">
    <div className="identity-grid" aria-hidden="true" />
    <section className="identity-card" aria-labelledby="identity-title">
      <div className="workspace-brand-mark identity-mark" aria-hidden="true">a<span>c</span></div>
      <div className="identity-eyebrow">AGENT / C</div>
      <h1 id="identity-title">{waiting ? "Opening your workspace" : member ? "Your account is signed in" : "From intent to verified work."}</h1>
      <p>{waiting ? "Checking your session…" : member ? "This workspace is available to its administrators. Sign in with an administrator account to continue." : "One workspace for autonomous agents, adaptable processes and connected devices."}</p>
      {session.isError || providers.isError || logoutError ? <div className="identity-error" role="alert">{logoutError || String(session.error ?? providers.error)}</div> : null}
      {!waiting && !member && !session.isError ? <div className="identity-providers">
        {([ ["google", "Google"], ["github", "GitHub"], ["apple", "Apple"] ] as const).map(([provider, name]) => <a key={provider} aria-disabled={providers.data?.[provider] !== true} href={providers.data?.[provider] === true ? `/auth/${provider}/start` : undefined} className="identity-provider">Continue with {name}<span aria-hidden="true">↗</span></a>)}
      </div> : null}
      {member ? <button className="identity-provider" disabled={signingOut} onClick={() => void signOut()}>Sign in with another account</button> : null}
      {session.isError || providers.isError ? <button className="workspace-button mt-4" onClick={() => { void session.refetch(); void providers.refetch(); }}>Retry</button> : null}
      <div className="identity-footer">Autonomous execution. Visible progress. Evidence that lasts.</div>
    </section>
  </main>;
}

export default function DeploymentAuthBoundary({ children }: { children: React.ReactNode }) {
  return window.__AGENT_UI_CONFIG__?.identityEnabled === true ? <SessionGate>{children}</SessionGate> : <>{children}</>;
}
