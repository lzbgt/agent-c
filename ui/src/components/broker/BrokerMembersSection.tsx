import React from "react";
import type { BrokerMembersResp } from "../../api";
import FieldLabel from "../FieldLabel";

type BrokerMember = BrokerMembersResp["members"][number];

function fmtTs(ms?: number | null) {
  if (!ms || !Number.isFinite(ms)) return "";
  try {
    return new Date(ms).toLocaleString();
  } catch {
    return String(ms);
  }
}

type BrokerMembersSectionProps = {
  canQuery: boolean;
  agentId: string;
  isFetching: boolean;
  error: unknown;
  members: BrokerMember[];
  ownerSub: string;
  deletePending: boolean;
  upsertPending: boolean;
  newUserSub: string;
  newRole: string;
  actionError: string | null;
  setNewUserSub: (next: string) => void;
  setNewRole: (next: string) => void;
  onRefresh: () => void;
  onUpsert: () => void;
  onDelete: (userSub: string) => void;
};

export default function BrokerMembersSection(props: BrokerMembersSectionProps) {
  const {
    canQuery,
    agentId,
    isFetching,
    error,
    members,
    ownerSub,
    deletePending,
    upsertPending,
    newUserSub,
    newRole,
    actionError,
    setNewUserSub,
    setNewRole,
    onRefresh,
    onUpsert,
    onDelete,
  } = props;

  return (
    <section className="rounded-md border border-white/10 bg-black/20 p-3">
      <div className="mb-2 flex items-center justify-between gap-2">
        <div className="text-xs font-semibold text-white/80">Members</div>
        <button
          className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
          type="button"
          disabled={!canQuery || !agentId || isFetching}
          onClick={onRefresh}
        >
          {isFetching ? "Loading…" : "Refresh"}
        </button>
      </div>

      {!agentId ? (
        <div className="text-[11px] text-white/50">Select an agent to manage membership.</div>
      ) : error ? (
        <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
          {String(error)}
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
                      disabled={isOwner || deletePending}
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
                disabled={!canQuery || !agentId || upsertPending}
                onClick={onUpsert}
              >
                {upsertPending ? "Saving…" : "Save"}
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
  );
}
