import React from "react";
import { useMutation, useQuery } from "@tanstack/react-query";
import {
  apiBrokerDeleteMember,
  apiBrokerGetMembers,
  apiBrokerGetMembershipAudit,
  apiBrokerListAgents,
  apiBrokerListDeployments,
  apiBrokerProxyJson,
  apiBrokerUpsertMember,
  type ApiAuth,
} from "../api";
import FieldLabel from "./FieldLabel";

const normalizeBrokerBase = (raw: string) => {
  const base = String(raw || "").trim();
  if (!base) return "";
  const withScheme = /^https?:\/\//i.test(base) ? base : `https://${base}`;
  return withScheme.replace(/\/+$/, "");
};

const fmtTs = (ms?: number | null) => {
  if (!ms || !Number.isFinite(ms)) return "";
  try {
    return new Date(ms).toLocaleString();
  } catch {
    return String(ms);
  }
};

export type BrokerPanelProps = {
  open: boolean;
  onToggle: (open: boolean) => void;
  brokerBase: string;
  brokerAgentId: string;
  setBrokerAgentId: (next: string) => void;
  auth: ApiAuth;
  authKey: string;
};

export default function BrokerPanel(props: BrokerPanelProps) {
  const base = React.useMemo(() => normalizeBrokerBase(props.brokerBase), [props.brokerBase]);
  const agentId = String(props.brokerAgentId || "").trim();
  const authToken = props.auth?.token ? String(props.auth.token).trim() : "";
  const canQuery = base.length > 0 && authToken.length > 0;

  const agentsQuery = useQuery({
    queryKey: ["brokerAgents", base, props.authKey],
    enabled: false,
    queryFn: () => apiBrokerListAgents(base, props.auth),
  });

  const membersQuery = useQuery({
    queryKey: ["brokerMembers", base, props.authKey, agentId],
    enabled: false,
    queryFn: () => apiBrokerGetMembers(base, agentId, props.auth),
  });

  const deploymentsQuery = useQuery({
    queryKey: ["brokerDeployments", base, props.authKey, agentId],
    enabled: false,
    queryFn: () => apiBrokerListDeployments(base, agentId, props.auth),
  });

  const normalizeDeploymentId = (raw: unknown) => {
    const id = String(raw || "").trim();
    return id || "default";
  };

  const [selectedDeployments, setSelectedDeployments] = React.useState<string[]>([]);
  const [otaUrl, setOtaUrl] = React.useState<string>("");
  const [otaSha256, setOtaSha256] = React.useState<string>("");
  const [otaVersion, setOtaVersion] = React.useState<string>("");
  const [otaDrainMs, setOtaDrainMs] = React.useState<string>("15000");
  const [otaReason, setOtaReason] = React.useState<string>("");
  const [otaBusy, setOtaBusy] = React.useState<boolean>(false);
  const [otaError, setOtaError] = React.useState<string | null>(null);
  const [otaResults, setOtaResults] = React.useState<any[] | null>(null);

  const [auditLimit, setAuditLimit] = React.useState<string>("200");
  const limitValue = React.useMemo(() => {
    const n = Number.parseInt(String(auditLimit || ""), 10);
    if (!Number.isFinite(n) || n <= 0) return 200;
    return Math.min(Math.max(n, 1), 500);
  }, [auditLimit]);

  const auditQuery = useQuery({
    queryKey: ["brokerMembershipAudit", base, props.authKey, agentId, limitValue],
    enabled: false,
    queryFn: () => apiBrokerGetMembershipAudit(base, agentId, limitValue, props.auth),
  });

  React.useEffect(() => {
    if (!props.open || !canQuery) return;
    if (!agentsQuery.data && !agentsQuery.isFetching) {
      void agentsQuery.refetch();
    }
  }, [props.open, canQuery, agentsQuery]);

  React.useEffect(() => {
    if (!props.open || !canQuery || !agentId) return;
    if (!membersQuery.data && !membersQuery.isFetching) {
      void membersQuery.refetch();
    }
    if (!auditQuery.data && !auditQuery.isFetching) {
      void auditQuery.refetch();
    }
    if (!deploymentsQuery.data && !deploymentsQuery.isFetching) {
      void deploymentsQuery.refetch();
    }
  }, [props.open, canQuery, agentId, membersQuery, auditQuery, deploymentsQuery]);

  React.useEffect(() => {
    if (!agentId) {
      setSelectedDeployments([]);
      return;
    }
    const deployments = Array.isArray((deploymentsQuery.data as any)?.deployments)
      ? (((deploymentsQuery.data as any).deployments as any[]) ?? [])
      : [];
    if (deployments.length === 0) {
      setSelectedDeployments([]);
      return;
    }
    const connected = deployments.filter((d) => d?.connected === true).map((d) => normalizeDeploymentId(d?.deployment_id));
    const all = deployments.map((d) => normalizeDeploymentId(d?.deployment_id));
    setSelectedDeployments(connected.length > 0 ? connected : all);
  }, [agentId, deploymentsQuery.data]);

  const upsertMutation = useMutation({
    mutationFn: async (req: { userSub: string; role: string }) => {
      const res = await apiBrokerUpsertMember(base, agentId, { user_sub: req.userSub, role: req.role }, props.auth);
      if (!res.ok) throw new Error(res.error || "upsert failed");
      return res;
    },
    onSuccess: () => {
      void membersQuery.refetch();
      void auditQuery.refetch();
    },
  });

  const deleteMutation = useMutation({
    mutationFn: async (userSub: string) => {
      const res = await apiBrokerDeleteMember(base, agentId, userSub, props.auth);
      if (!res.ok) throw new Error(res.error || "delete failed");
      return res;
    },
    onSuccess: () => {
      void membersQuery.refetch();
      void auditQuery.refetch();
    },
  });

  const [newUserSub, setNewUserSub] = React.useState<string>("");
  const [newRole, setNewRole] = React.useState<string>("user");
  const [actionError, setActionError] = React.useState<string | null>(null);

  const onUpsert = async () => {
    setActionError(null);
    const userSub = String(newUserSub || "").trim();
    if (!userSub) {
      setActionError("missing user_sub");
      return;
    }
    const role = String(newRole || "user").trim().toLowerCase();
    if (role !== "user" && role !== "admin" && role !== "owner") {
      setActionError("invalid role");
      return;
    }
    try {
      await upsertMutation.mutateAsync({ userSub, role });
      setNewUserSub("");
    } catch (e) {
      setActionError(String(e));
    }
  };

  const onDelete = async (userSub: string) => {
    setActionError(null);
    if (!window.confirm(`Remove ${userSub}?`)) return;
    try {
      await deleteMutation.mutateAsync(userSub);
    } catch (e) {
      setActionError(String(e));
    }
  };

  const toggleDeployment = (id: string) => {
    setSelectedDeployments((prev) => {
      const next = new Set(prev);
      if (next.has(id)) next.delete(id);
      else next.add(id);
      return Array.from(next);
    });
  };

  const selectAllDeployments = () => {
    setSelectedDeployments(deployments.map((d) => normalizeDeploymentId(d?.deployment_id)));
  };

  const selectConnectedDeployments = () => {
    const connected = deployments.filter((d) => d?.connected === true).map((d) => normalizeDeploymentId(d?.deployment_id));
    setSelectedDeployments(connected.length > 0 ? connected : deployments.map((d) => normalizeDeploymentId(d?.deployment_id)));
  };

  const runOtaUpdate = async () => {
    setOtaError(null);
    setOtaResults(null);
    const url = String(otaUrl || "").trim();
    if (!url) {
      setOtaError("missing OTA url");
      return;
    }
    if (!agentId) {
      setOtaError("missing agent_id");
      return;
    }
    if (selectedDeployments.length === 0) {
      setOtaError("select at least one deployment");
      return;
    }
    const drainMs = Number.parseInt(String(otaDrainMs || ""), 10);
    const drainTimeout = Number.isFinite(drainMs) && drainMs >= 0 ? drainMs : undefined;
    const body: Record<string, any> = {
      url,
    };
    const sha = String(otaSha256 || "").trim();
    if (sha) body.sha256 = sha;
    const ver = String(otaVersion || "").trim();
    if (ver) body.version = ver;
    const reason = String(otaReason || "").trim();
    if (reason) body.reason = reason;
    if (drainTimeout !== undefined) body.drain_timeout_ms = drainTimeout;

    setOtaBusy(true);
    try {
      const settled = await Promise.allSettled(
        selectedDeployments.map(async (deploymentId) => {
          const res = await apiBrokerProxyJson(base, agentId, "/api/v1/ota/update", "POST", body, props.auth, deploymentId);
          return {
            deployment_id: deploymentId,
            status: res.status,
            data: res.data,
          };
        }),
      );
      const results = settled.map((r, idx) => {
        if (r.status === "fulfilled") return r.value;
        return {
          deployment_id: selectedDeployments[idx],
          status: 0,
          data: { ok: false, error: String(r.reason || "request failed") },
        };
      });
      setOtaResults(results);
    } catch (e) {
      setOtaError(String(e));
    } finally {
      setOtaBusy(false);
    }
  };

  const agents = Array.isArray((agentsQuery.data as any)?.agents) ? ((agentsQuery.data as any).agents as any[]) : [];
  const members = Array.isArray((membersQuery.data as any)?.members) ? ((membersQuery.data as any).members as any[]) : [];
  const ownerSub = String((membersQuery.data as any)?.owner_sub || "");
  const auditRows = Array.isArray((auditQuery.data as any)?.audit) ? ((auditQuery.data as any).audit as any[]) : [];
  const deployments = Array.isArray((deploymentsQuery.data as any)?.deployments)
    ? (((deploymentsQuery.data as any).deployments as any[]) ?? [])
    : [];
  const defaultDeploymentIdRaw = (deploymentsQuery.data as any)?.default_deployment_id;
  const defaultDeploymentId = defaultDeploymentIdRaw ? normalizeDeploymentId(defaultDeploymentIdRaw) : "";
  const selectedDeploymentSet = new Set(selectedDeployments);

  return (
    <details
      className="mb-4 rounded-lg border border-white/10 bg-white/5 px-3 py-2"
      open={props.open}
      onToggle={(ev) => props.onToggle((ev.currentTarget as HTMLDetailsElement).open)}
    >
      <summary className="cursor-pointer select-none text-xs text-white/80">
        <div className="flex flex-wrap items-center justify-between gap-2">
          <div className="font-semibold text-white/80">Broker console</div>
          <div className="text-[11px] text-white/50">Manage agents + membership when in broker mode</div>
        </div>
      </summary>

      <div className="mt-3 grid gap-4">
        {!base ? (
          <div className="rounded-md border border-amber-500/30 bg-amber-500/10 px-3 py-2 text-xs text-amber-100">
            Missing broker base URL. Set it in Settings.
          </div>
        ) : authToken.length === 0 ? (
          <div className="rounded-md border border-amber-500/30 bg-amber-500/10 px-3 py-2 text-xs text-amber-100">
            Missing broker auth token (OIDC). Set it in Settings.
          </div>
        ) : null}

        <section className="rounded-md border border-white/10 bg-black/20 p-3">
          <div className="mb-2 flex items-center justify-between gap-2">
            <div className="text-xs font-semibold text-white/80">Agents</div>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
              type="button"
              disabled={!canQuery || agentsQuery.isFetching}
              onClick={() => void agentsQuery.refetch()}
            >
              {agentsQuery.isFetching ? "Loading…" : "Refresh"}
            </button>
          </div>

          {agentsQuery.error ? (
            <div className="mb-2 rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
              {String(agentsQuery.error)}
            </div>
          ) : null}

          {agents.length === 0 ? (
            <div className="text-[11px] text-white/50">No agents returned.</div>
          ) : (
            <div className="grid gap-2">
              {agents.map((agent) => {
                const id = String(agent?.agent_id || "");
                const connected = agent?.connected === true;
                const selected = id && id === agentId;
                return (
                  <div key={id} className="flex flex-wrap items-center justify-between gap-2 rounded-md border border-white/5 bg-black/30 px-2 py-1">
                    <div className="flex flex-col">
                      <div className="text-xs text-white/90">{id}</div>
                      <div className="text-[11px] text-white/50">
                        {connected ? "connected" : "disconnected"}
                        {agent?.owner_sub ? ` · owner ${String(agent.owner_sub)}` : ""}
                      </div>
                    </div>
                    <button
                      className={
                        selected
                          ? "rounded-md border border-emerald-400/40 bg-emerald-500/10 px-2 py-1 text-[11px] text-emerald-100"
                          : "rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                      }
                      type="button"
                      onClick={() => props.setBrokerAgentId(id)}
                    >
                      {selected ? "Selected" : "Use"}
                    </button>
                  </div>
                );
              })}
            </div>
          )}
        </section>

        <section className="rounded-md border border-white/10 bg-black/20 p-3">
          <div className="mb-2 flex items-center justify-between gap-2">
            <div className="text-xs font-semibold text-white/80">Members</div>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
              type="button"
              disabled={!canQuery || !agentId || membersQuery.isFetching}
              onClick={() => void membersQuery.refetch()}
            >
              {membersQuery.isFetching ? "Loading…" : "Refresh"}
            </button>
          </div>

          {!agentId ? (
            <div className="text-[11px] text-white/50">Select an agent to manage membership.</div>
          ) : membersQuery.error ? (
            <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
              {String(membersQuery.error)}
            </div>
          ) : (
            <>
              <div className="mb-2 text-[11px] text-white/50">Owner: {ownerSub || "(unknown)"}</div>
              <div className="grid gap-2">
                {members.length === 0 ? (
                  <div className="text-[11px] text-white/50">No members.</div>
                ) : (
                  members.map((member) => {
                    const userSub = String(member?.user_sub || "");
                    const role = String(member?.role || "user");
                    const created = fmtTs(member?.created_unix_ms);
                    const isOwner = role === "owner" || userSub === ownerSub;
                    return (
                      <div
                        key={`${userSub}-${role}`}
                        className="flex flex-wrap items-center justify-between gap-2 rounded-md border border-white/5 bg-black/30 px-2 py-1"
                      >
                        <div className="flex flex-col">
                          <div className="text-xs text-white/90">{userSub}</div>
                          <div className="text-[11px] text-white/50">
                            role: {role}
                            {created ? ` · added ${created}` : ""}
                          </div>
                        </div>
                        <button
                          className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40 disabled:opacity-50"
                          type="button"
                          disabled={isOwner || deleteMutation.isPending}
                          title={isOwner ? "Owner cannot be removed." : "Remove member"}
                          onClick={() => onDelete(userSub)}
                        >
                          Remove
                        </button>
                      </div>
                    );
                  })
                )}
              </div>

              <div className="mt-3 grid gap-2">
                <FieldLabel>Add / update member</FieldLabel>
                <div className="flex flex-wrap items-center gap-2">
                  <input
                    className="min-w-[220px] flex-1 rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40"
                    placeholder="user_sub"
                    value={newUserSub}
                    onChange={(e) => setNewUserSub(e.target.value)}
                  />
                  <select
                    className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
                    value={newRole}
                    onChange={(e) => setNewRole(e.target.value)}
                  >
                    <option value="user">user</option>
                    <option value="admin">admin</option>
                  </select>
                  <button
                    className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50"
                    type="button"
                    disabled={!canQuery || !agentId || upsertMutation.isPending}
                    onClick={() => void onUpsert()}
                  >
                    {upsertMutation.isPending ? "Saving…" : "Save"}
                  </button>
                </div>
                {actionError ? (
                  <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
                    {actionError}
                  </div>
                ) : null}
              </div>
            </>
          )}
        </section>

        <section className="rounded-md border border-white/10 bg-black/20 p-3">
          <div className="mb-2 flex items-center justify-between gap-2">
            <div className="text-xs font-semibold text-white/80">Deployments + OTA</div>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
              type="button"
              disabled={!canQuery || !agentId || deploymentsQuery.isFetching}
              onClick={() => void deploymentsQuery.refetch()}
            >
              {deploymentsQuery.isFetching ? "Loading…" : "Refresh"}
            </button>
          </div>

          {!agentId ? (
            <div className="text-[11px] text-white/50">Select an agent to manage deployments.</div>
          ) : deploymentsQuery.error ? (
            <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
              {String(deploymentsQuery.error)}
            </div>
          ) : deployments.length === 0 ? (
            <div className="text-[11px] text-white/50">No deployments connected.</div>
          ) : (
            <>
              {defaultDeploymentId ? (
                <div className="mb-2 text-[11px] text-white/50">
                  Broker default: <span className="font-mono text-white/80">{defaultDeploymentId}</span>
                </div>
              ) : null}
              <div className="grid gap-2">
                {deployments.map((dep) => {
                  const id = normalizeDeploymentId(dep?.deployment_id);
                  const selected = selectedDeploymentSet.has(id);
                  const connected = dep?.connected === true;
                  return (
                    <label
                      key={id}
                      className="flex flex-wrap items-center justify-between gap-2 rounded-md border border-white/5 bg-black/30 px-2 py-1"
                    >
                      <div className="flex items-center gap-2">
                        <input
                          type="checkbox"
                          checked={selected}
                          onChange={() => toggleDeployment(id)}
                        />
                        <div className="flex flex-col">
                          <div className="text-xs text-white/90">{id}</div>
                          <div className="text-[11px] text-white/50">
                            {connected ? "connected" : "disconnected"}
                          </div>
                        </div>
                      </div>
                      <button
                        className={
                          selected
                            ? "rounded-md border border-emerald-400/40 bg-emerald-500/10 px-2 py-1 text-[11px] text-emerald-100"
                            : "rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                        }
                        type="button"
                        onClick={() => toggleDeployment(id)}
                      >
                        {selected ? "Selected" : "Select"}
                      </button>
                    </label>
                  );
                })}
              </div>
              <div className="mt-2 flex flex-wrap items-center gap-2">
                <button
                  className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                  type="button"
                  onClick={selectConnectedDeployments}
                >
                  Select connected
                </button>
                <button
                  className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                  type="button"
                  onClick={selectAllDeployments}
                >
                  Select all
                </button>
              </div>
            </>
          )}

          <div className="mt-3 grid gap-2">
            <FieldLabel>OTA update</FieldLabel>
            <input
              className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40"
              placeholder="https://.../agentd.tar.gz"
              value={otaUrl}
              onChange={(e) => setOtaUrl(e.target.value)}
            />
            <div className="grid grid-cols-1 gap-2 md:grid-cols-2">
              <input
                className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40"
                placeholder="sha256 (optional)"
                value={otaSha256}
                onChange={(e) => setOtaSha256(e.target.value)}
              />
              <input
                className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40"
                placeholder="version label (optional)"
                value={otaVersion}
                onChange={(e) => setOtaVersion(e.target.value)}
              />
            </div>
            <div className="grid grid-cols-1 gap-2 md:grid-cols-2">
              <input
                className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40"
                placeholder="drain timeout ms (default 15000)"
                value={otaDrainMs}
                onChange={(e) => setOtaDrainMs(e.target.value)}
              />
              <input
                className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40"
                placeholder="reason (optional)"
                value={otaReason}
                onChange={(e) => setOtaReason(e.target.value)}
              />
            </div>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50"
              type="button"
              disabled={!agentId || otaBusy || selectedDeployments.length === 0}
              onClick={() => void runOtaUpdate()}
            >
              {otaBusy ? "Updating…" : "Run OTA update"}
            </button>
            {otaError ? (
              <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
                {otaError}
              </div>
            ) : null}
            {otaResults && otaResults.length > 0 ? (
              <div className="grid gap-2">
                {otaResults.map((row) => {
                  const depId = String(row?.deployment_id || "");
                  const status = row?.status;
                  const ok = row?.data?.ok === true;
                  const err = row?.data?.error || row?.data?.err;
                  const respStatus = row?.data?.status || "";
                  return (
                    <div
                      key={`ota-${depId}`}
                      className="rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70"
                    >
                      <div className="text-xs text-white/90">
                        {depId} · {ok ? "ok" : "error"} · http {status}
                      </div>
                      <div className="text-[11px] text-white/50">
                        {respStatus ? `status ${respStatus}` : "no status"}
                        {err ? ` · ${String(err)}` : ""}
                      </div>
                    </div>
                  );
                })}
              </div>
            ) : null}
          </div>
        </section>

        <section className="rounded-md border border-white/10 bg-black/20 p-3">
          <div className="mb-2 flex items-center justify-between gap-2">
            <div className="text-xs font-semibold text-white/80">Membership audit</div>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
              type="button"
              disabled={!canQuery || !agentId || auditQuery.isFetching}
              onClick={() => void auditQuery.refetch()}
            >
              {auditQuery.isFetching ? "Loading…" : "Refresh"}
            </button>
          </div>
          <div className="mb-2 flex flex-wrap items-center gap-2">
            <FieldLabel>Limit</FieldLabel>
            <input
              className="w-24 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={auditLimit}
              onChange={(e) => setAuditLimit(e.target.value)}
            />
            <span className="text-[11px] text-white/50">(1-500)</span>
          </div>

          {!agentId ? (
            <div className="text-[11px] text-white/50">Select an agent to view audit history.</div>
          ) : auditQuery.error ? (
            <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
              {String(auditQuery.error)}
            </div>
          ) : auditRows.length === 0 ? (
            <div className="text-[11px] text-white/50">No audit rows.</div>
          ) : (
            <div className="grid gap-2">
              {auditRows.map((row, idx) => {
                const action = String(row?.action || "");
                const actor = String(row?.actor_sub || "");
                const target = String(row?.target_sub || "");
                const role = String(row?.role || "");
                const traceId = String(row?.trace_id || "");
                const ts = fmtTs(row?.ts_unix_ms);
                return (
                  <div
                    key={`${actor}-${target}-${action}-${idx}`}
                    className="rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70"
                  >
                    <div className="text-xs text-white/90">{action || "update"}</div>
                    <div className="text-[11px] text-white/50">
                      actor {actor} → target {target}
                      {role ? ` · role ${role}` : ""}
                      {traceId ? ` · trace ${traceId}` : ""}
                      {ts ? ` · ${ts}` : ""}
                    </div>
                  </div>
                );
              })}
            </div>
          )}
        </section>
      </div>
    </details>
  );
}
