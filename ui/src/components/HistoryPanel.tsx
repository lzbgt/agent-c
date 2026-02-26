import React from "react";
import type { ApiAuth } from "../api";
import ConversationView from "./ConversationView";
import Markdown from "./Markdown";
import ArtifactView from "./ArtifactView";
import type { SceneEntity } from "./SceneView";
import useLocalStorageState from "../hooks/useLocalStorageState";

type HighlightSnippet = {
  snippet: string;
  start: number;
  end: number;
};

function buildHighlightSnippet(text: string, needle: string, maxLen = 180): HighlightSnippet | null {
  const cleanNeedle = needle.trim();
  if (!text || !cleanNeedle) return null;
  const lowerText = text.toLowerCase();
  const lowerNeedle = cleanNeedle.toLowerCase();
  const idx = lowerText.indexOf(lowerNeedle);
  if (idx < 0) return null;
  const pad = 40;
  let start = Math.max(0, idx - pad);
  let end = Math.min(text.length, idx + lowerNeedle.length + pad);
  if (end - start < maxLen) {
    const extra = maxLen - (end - start);
    start = Math.max(0, start - Math.floor(extra / 2));
    end = Math.min(text.length, end + Math.ceil(extra / 2));
  }
  return { snippet: text.slice(start, end), start, end };
}

function renderHighlightedSnippet(snippet: string, needle: string): React.ReactNode {
  const cleanNeedle = needle.trim();
  if (!snippet || !cleanNeedle) return snippet;
  const lowerSnippet = snippet.toLowerCase();
  const lowerNeedle = cleanNeedle.toLowerCase();
  const parts: React.ReactNode[] = [];
  let cursor = 0;
  while (true) {
    const idx = lowerSnippet.indexOf(lowerNeedle, cursor);
    if (idx < 0) break;
    if (idx > cursor) parts.push(snippet.slice(cursor, idx));
    parts.push(
      <mark key={`${idx}-${cursor}`} className="rounded bg-amber-400/30 px-0.5 text-amber-100">
        {snippet.slice(idx, idx + lowerNeedle.length)}
      </mark>,
    );
    cursor = idx + lowerNeedle.length;
  }
  if (cursor < snippet.length) parts.push(snippet.slice(cursor));
  return parts;
}

export type HistoryPanelProps = {
  entries: any[];
  showAllEntries: boolean;
  setShowAllEntries: React.Dispatch<React.SetStateAction<boolean>>;
  showMessages: boolean;
  setShowMessages: React.Dispatch<React.SetStateAction<boolean>>;
  historyExpandedByKey: Record<string, boolean>;
  setHistoryExpandedByKey: React.Dispatch<React.SetStateAction<Record<string, boolean>>>;
  dbMessages?: any[];
  dbRuns?: any[];
  dbRunDetailsById?: Record<number, any>;
  sessionArtifacts?: any[];
  effectiveBase: string;
  yolo: boolean;
  sessionId: string;
  client: { id: string; kind: string; instance_id: string };
  daemonAuth: ApiAuth;
  showDebugInConversation: boolean;
  allowAutoplay: boolean;
  allowClientRpcs: boolean;
  allowClientEffects: boolean;
  allowUnsafePageEval: boolean;
  sceneEntities: SceneEntity[];
  onSceneApply: (ops: any[]) => void;
  onTraceIdClick: (traceId: string) => void;
  teamConversationItems?: any[];
  teamId?: string;
  teamRunId?: string;
  teamRunCreatedMs?: number;
  teamRunStatus?: string;
  teamConversationWarnings?: string[];
};

export default function HistoryPanel(props: HistoryPanelProps) {
  const entries = Array.isArray(props.entries) ? props.entries : [];
  const dbMessages = Array.isArray(props.dbMessages) ? props.dbMessages : [];
  const dbRuns = Array.isArray(props.dbRuns) ? props.dbRuns : [];
  const dbRunDetailsById =
    props.dbRunDetailsById && typeof props.dbRunDetailsById === "object" ? props.dbRunDetailsById : {};
  const sessionArtifacts = Array.isArray(props.sessionArtifacts) ? props.sessionArtifacts : [];
  const teamConversationItems = Array.isArray(props.teamConversationItems) ? props.teamConversationItems : [];
  const teamId = String(props.teamId || "").trim();
  const teamRunId = String(props.teamRunId || "").trim();
  const teamRunCreatedMs = typeof props.teamRunCreatedMs === "number" ? props.teamRunCreatedMs : 0;
  const teamRunStatus = String(props.teamRunStatus || "").trim();
  const teamConversationWarnings = Array.isArray(props.teamConversationWarnings) ? props.teamConversationWarnings : [];
  const teamAgentSummary = React.useMemo(() => {
    const items = teamConversationItems.slice().sort((a, b) => (a?.ts || 0) - (b?.ts || 0));
    const agents = new Map<string, { lastTs: number; lastContent: string }>();
    for (const item of items) {
      const meta = item?.meta ?? {};
      const agentId = typeof meta?.agent_id === "string" ? meta.agent_id : "";
      if (!agentId) continue;
      const ts = typeof item?.ts === "number" ? item.ts : 0;
      const content = typeof item?.message?.content === "string" ? item.message.content : "";
      const prev = agents.get(agentId);
      if (!prev || ts >= prev.lastTs) {
        agents.set(agentId, { lastTs: ts, lastContent: content });
      }
    }
    return Array.from(agents.entries())
      .map(([agentId, value]) => ({
        agentId,
        lastTs: value.lastTs,
        lastContent: value.lastContent,
      }))
      .sort((a, b) => b.lastTs - a.lastTs);
  }, [teamConversationItems]);
  const teamLastActivity = teamAgentSummary.length > 0 ? teamAgentSummary[0].lastTs : 0;
  const teamSummaryKey = React.useMemo(() => {
    const base = String(props.effectiveBase || "").trim() || "default";
    const tid = teamId || "none";
    return `agentui.teamSummaryExpanded:${base}::${tid}`;
  }, [props.effectiveBase, teamId]);
  const [showAllTeamAgents, setShowAllTeamAgents] = useLocalStorageState<boolean>(teamSummaryKey, false);
  const teamGroupStateKey = React.useMemo(() => {
    const base = String(props.effectiveBase || "").trim() || "default";
    const tid = teamId || "none";
    return `agentui.teamGroupState:${base}::${tid}`;
  }, [props.effectiveBase, teamId]);
  const [teamGroupState, setTeamGroupState] = useLocalStorageState<Record<string, boolean>>(teamGroupStateKey, {});
  const teamFiltersCollapsedKey = React.useMemo(() => {
    const base = String(props.effectiveBase || "").trim() || "default";
    const tid = teamId || "none";
    return `agentui.teamFiltersCollapsed:${base}::${tid}`;
  }, [props.effectiveBase, teamId]);
  const [teamFiltersCollapsed, setTeamFiltersCollapsed] = useLocalStorageState<boolean>(teamFiltersCollapsedKey, false);
  const MAX_HISTORY_EXPANDED_KEYS = 200;
  const technicalHistoryKey = React.useMemo(() => {
    const base = String(props.effectiveBase || "").trim() || "default";
    const sid = String(props.sessionId || "").trim() || "default";
    return `agentui.technicalHistory:${base}::${sid}`;
  }, [props.effectiveBase, props.sessionId]);
  const [showTechnicalHistory, setShowTechnicalHistory] = useLocalStorageState<boolean>(technicalHistoryKey, false);
  const teamChatKey = React.useMemo(() => {
    const base = String(props.effectiveBase || "").trim() || "default";
    const tid = teamId || "none";
    return `agentui.teamChat:${base}::${tid}`;
  }, [props.effectiveBase, teamId]);
  const [showTeamChat, setShowTeamChat] = useLocalStorageState<boolean>(teamChatKey, true);
  const teamRunMarkersKey = React.useMemo(() => {
    const base = String(props.effectiveBase || "").trim() || "default";
    const tid = teamId || "none";
    return `agentui.teamRunMarkers:${base}::${tid}`;
  }, [props.effectiveBase, teamId]);
  const [showTeamRunMarkers, setShowTeamRunMarkers] = useLocalStorageState<boolean>(teamRunMarkersKey, true);
  const teamGroupByAgentKey = React.useMemo(() => {
    const base = String(props.effectiveBase || "").trim() || "default";
    const tid = teamId || "none";
    return `agentui.teamGroupByAgent:${base}::${tid}`;
  }, [props.effectiveBase, teamId]);
  const [showTeamGroupByAgent, setShowTeamGroupByAgent] = useLocalStorageState<boolean>(teamGroupByAgentKey, false);
  const teamRoleLabelsKey = React.useMemo(() => {
    const base = String(props.effectiveBase || "").trim() || "default";
    const tid = teamId || "none";
    return `agentui.teamRoleLabels:${base}::${tid}`;
  }, [props.effectiveBase, teamId]);
  const teamHeadersKey = React.useMemo(() => {
    const base = String(props.effectiveBase || "").trim() || "default";
    const tid = teamId || "none";
    return `agentui.teamHeaders:${base}::${tid}`;
  }, [props.effectiveBase, teamId]);
  const teamSystemKey = React.useMemo(() => {
    const base = String(props.effectiveBase || "").trim() || "default";
    const tid = teamId || "none";
    return `agentui.teamSystemMessages:${base}::${tid}`;
  }, [props.effectiveBase, teamId]);
  const [showTeamRoleLabels, setShowTeamRoleLabels] = useLocalStorageState<boolean>(teamRoleLabelsKey, true);
  const [showTeamHeaders, setShowTeamHeaders] = useLocalStorageState<boolean>(teamHeadersKey, false);
  const [showTeamSystemMessages, setShowTeamSystemMessages] = useLocalStorageState<boolean>(teamSystemKey, false);
  const teamMutedAgentsKey = React.useMemo(() => {
    const base = String(props.effectiveBase || "").trim() || "default";
    const tid = teamId || "none";
    return `agentui.teamMutedAgents:${base}::${tid}`;
  }, [props.effectiveBase, teamId]);
  const [teamMutedAgents, setTeamMutedAgents] = useLocalStorageState<string[]>(teamMutedAgentsKey, []);
  const mutedAgentSet = React.useMemo(() => new Set(teamMutedAgents || []), [teamMutedAgents]);
  const teamSearchKey = React.useMemo(() => {
    const base = String(props.effectiveBase || "").trim() || "default";
    const tid = teamId || "none";
    return `agentui.teamSearch:${base}::${tid}`;
  }, [props.effectiveBase, teamId]);
  const [teamSearch, setTeamSearch] = useLocalStorageState<string>(teamSearchKey, "");
  const teamSavedFiltersKey = React.useMemo(() => {
    const base = String(props.effectiveBase || "").trim() || "default";
    const tid = teamId || "none";
    return `agentui.teamSavedFilters:${base}::${tid}`;
  }, [props.effectiveBase, teamId]);
  const [teamSavedFilters, setTeamSavedFilters] = useLocalStorageState<string[]>(teamSavedFiltersKey, []);
  const teamPinnedFiltersKey = React.useMemo(() => {
    const base = String(props.effectiveBase || "").trim() || "default";
    const tid = teamId || "none";
    return `agentui.teamPinnedFilters:${base}::${tid}`;
  }, [props.effectiveBase, teamId]);
  const [teamPinnedFilters, setTeamPinnedFilters] = useLocalStorageState<string[]>(teamPinnedFiltersKey, []);
  const teamSearchRef = React.useRef<HTMLInputElement | null>(null);
  const teamRoleChips = React.useMemo(() => {
    const roles = new Set<string>();
    for (const item of teamConversationItems) {
      const role = typeof item?.meta?.role === "string" ? item.meta.role : "";
      if (role) roles.add(role);
    }
    return Array.from(roles.values()).slice(0, 6);
  }, [teamConversationItems]);
  const filtersActive = Boolean(
    teamSearch.trim().length > 0 || teamMutedAgents.length > 0 || showTeamSystemMessages || !showTeamRunMarkers,
  );

  const teamTimelineItems = React.useMemo(() => {
    const items = teamConversationItems
      .slice()
      .filter((item) => {
        const role = typeof item?.message?.role === "string" ? item.message.role : "";
        if (role === "system" && !showTeamSystemMessages) return false;
        const agentLabel = typeof item?.meta?.agent_id === "string" ? item.meta.agent_id : "";
        if (agentLabel && mutedAgentSet.has(agentLabel)) return false;
        const needle = String(teamSearch || "").trim().toLowerCase();
        if (needle) {
          const content = typeof item?.message?.content === "string" ? item.message.content : "";
          const haystack = `${content} ${agentLabel} ${role}`.toLowerCase();
          if (!haystack.includes(needle)) return false;
        }
        return true;
      })
      .sort((a, b) => (a?.ts || 0) - (b?.ts || 0));
    const out: Array<{ kind: "marker" | "item"; runId?: string; item?: any; ts: number }> = [];
    let lastRunId = "";
    for (const item of items) {
      const ts = typeof item?.ts === "number" ? item.ts : 0;
      const runIdRaw = item?.meta?.run_id;
      const runId = typeof runIdRaw === "string" ? runIdRaw : String(runIdRaw || "").trim();
      if (showTeamRunMarkers && runId && runId !== lastRunId) {
        out.push({ kind: "marker", runId, ts });
        lastRunId = runId;
      }
      out.push({ kind: "item", item, ts });
    }
    return out;
  }, [mutedAgentSet, showTeamRunMarkers, showTeamSystemMessages, teamConversationItems]);

  const teamFilteredItems = React.useMemo(() => {
    return teamTimelineItems.filter((entry) => entry.kind === "item");
  }, [teamTimelineItems]);
  const teamFilteredCount = teamFilteredItems.length;

  const teamGroupedByAgent = React.useMemo(() => {
    if (!showTeamGroupByAgent) return [] as Array<{ key: string; label: string; items: any[]; latest: number; preview: string }>;
    const groups = new Map<string, { label: string; items: any[]; latest: number }>();
    for (const entry of teamTimelineItems) {
      if (entry.kind !== "item") continue;
      const item = entry.item;
      const meta = item?.meta ?? {};
      const agentLabel = typeof meta?.agent_id === "string" && meta.agent_id ? meta.agent_id : "";
      const roleLabel = typeof meta?.role === "string" && meta.role ? meta.role : "";
      const baseLabel = agentLabel || roleLabel || "unknown";
      const label = agentLabel && roleLabel ? `${agentLabel} · ${roleLabel}` : baseLabel;
      const key = baseLabel;
      const ts = typeof item?.ts === "number" ? item.ts : 0;
      if (!groups.has(key)) {
        groups.set(key, { label, items: [item], latest: ts });
      } else {
        const g = groups.get(key)!;
        g.items.push(item);
        if (ts > g.latest) g.latest = ts;
      }
    }
    return Array.from(groups.entries())
      .map(([key, value]) => {
        const sorted = value.items
          .slice()
          .sort((a, b) => (a?.ts || 0) - (b?.ts || 0));
        const last = sorted[sorted.length - 1];
        const lastContent = typeof last?.message?.content === "string" ? last.message.content : "";
        const preview =
          lastContent.trim().length > 0
            ? lastContent.trim().slice(0, 120)
            : "(no content)";
        return { key, ...value, items: sorted, preview };
      })
      .sort((a, b) => b.latest - a.latest);
  }, [showTeamGroupByAgent, teamTimelineItems]);

  const renderTeamMessage = React.useCallback(
    (item: any, idx: number) => {
      const msg = item?.message ?? {};
      const role = typeof msg?.role === "string" ? msg.role : "message";
      const content = typeof msg?.content === "string" ? msg.content : "";
      const ts = typeof item?.ts === "number" ? item.ts : 0;
      const when = ts ? new Date(ts).toLocaleString() : "";
      const meta = item?.meta ?? {};
      const roleLabel = typeof meta?.role === "string" && meta.role ? meta.role : role;
      const agentLabel = typeof meta?.agent_id === "string" && meta.agent_id ? meta.agent_id : "";
      const roleBadge =
        roleLabel === "guidance"
          ? "border-amber-400/30 bg-amber-500/10 text-amber-100"
          : roleLabel === "goal"
            ? "border-emerald-400/30 bg-emerald-500/10 text-emerald-100"
            : roleLabel === "user"
              ? "border-indigo-400/30 bg-indigo-500/10 text-indigo-100"
              : "border-white/10 bg-black/30 text-white/70";
      const isSystem = role === "system" || roleLabel === "system";
      const cardAccent =
        roleLabel === "guidance"
          ? "border-amber-400/30 bg-amber-500/5"
          : roleLabel === "goal"
            ? "border-emerald-400/30 bg-emerald-500/5"
            : roleLabel === "user"
              ? "border-indigo-400/30 bg-indigo-500/5"
              : "border-white/10 bg-white/5";
      if (agentLabel && mutedAgentSet.has(agentLabel)) return null;
      const sessionLabel = typeof meta?.session_id === "string" ? meta.session_id : "";
      const hasMeta = !!agentLabel || !!sessionLabel;
      const payload = meta?.payload ?? {};
      const uploads = Array.isArray(payload?.uploads) ? payload.uploads : [];
      const uploadFiles = uploads.flatMap((u: any) => (Array.isArray(u?.files) ? u.files : []));
      const uploadNames = uploadFiles
        .map((f: any) => String(f?.name || f?.path || "").trim())
        .filter((f: string) => f.length > 0);
      const highlightNeedle = teamSearch.trim();
      const highlightSnippet = highlightNeedle ? buildHighlightSnippet(content, highlightNeedle) : null;
      const compactLabel =
        agentLabel || (roleLabel === "user" ? "You" : roleLabel && roleLabel !== "assistant" ? roleLabel : "");
      const showRoleBadges = showTeamHeaders && showTeamRoleLabels;
      return (
        <div
          key={`team-msg:${ts || idx}`}
          id={ts ? `team-msg-${ts}` : undefined}
          className={`rounded-lg border px-3 py-2 ${cardAccent}`}
        >
          {showTeamHeaders ? (
            <div className="flex flex-wrap items-center gap-2 text-[11px] text-white/60">
              {agentLabel ? (
                <span className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 font-semibold text-white/80">
                  {agentLabel}
                </span>
              ) : roleLabel === "user" ? (
                <span className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 font-semibold text-white/80">
                  You
                </span>
              ) : null}
              {showRoleBadges ? (
                <span className={`rounded-md border px-2 py-0.5 text-[11px] ${roleBadge}`}>
                  {roleLabel === "user" ? "user" : roleLabel}
                </span>
              ) : null}
              {when ? <span>{when}</span> : null}
            </div>
          ) : compactLabel ? (
            <div className="text-[11px] text-white/60">
              <span className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 font-semibold text-white/80">
                {compactLabel}
              </span>
            </div>
          ) : null}
          {highlightSnippet ? (
            <div className="mt-2 text-[11px] text-amber-100">
              <span className="text-amber-200">Match:</span>{" "}
              {highlightSnippet.start > 0 ? "…" : null}
              {renderHighlightedSnippet(highlightSnippet.snippet, highlightNeedle)}
              {highlightSnippet.end < content.length ? "…" : null}
            </div>
          ) : null}
          {content ? (
            isSystem ? (
              <details className="mt-2">
                <summary className="cursor-pointer text-[11px] text-white/60">System message (collapsed)</summary>
                <div className="mt-2 text-sm text-white/90">
                  <Markdown text={String(content)} />
                </div>
              </details>
            ) : (
              <div className="mt-2 text-sm text-white/90">
                <Markdown text={String(content)} />
              </div>
            )
          ) : null}
          {hasMeta ? (
            <details className="mt-2">
              <summary className="cursor-pointer text-[11px] text-white/60">Message details</summary>
              <div className="mt-2 text-[11px] text-white/60">
                {agentLabel ? <div>agent {agentLabel}</div> : null}
                {sessionLabel ? <div>session {sessionLabel}</div> : null}
              </div>
            </details>
          ) : null}
          {uploadNames.length > 0 ? (
            <div className="mt-2 text-[11px] text-white/60">
              Shared files: <span className="text-white/80">{uploadNames.join(", ")}</span>
            </div>
          ) : null}
        </div>
      );
    },
    [mutedAgentSet, showTeamHeaders, showTeamRoleLabels, teamSearch],
  );

  const jumpToMatch = React.useCallback(
    (direction: "first" | "last") => {
      const items = teamFilteredItems;
      if (items.length === 0) return;
      const entry = direction === "first" ? items[0] : items[items.length - 1];
      const ts = typeof entry?.ts === "number" ? entry.ts : 0;
      if (!ts) return;
      const el = document.getElementById(`team-msg-${ts}`);
      if (el) {
        el.scrollIntoView({ behavior: "smooth", block: "center" });
      }
    },
    [teamFilteredItems],
  );

  React.useEffect(() => {
    const lastKeyRef = { key: "", ts: 0 };
    const shouldIgnoreTarget = (target: EventTarget | null) => {
      if (!target || !(target as HTMLElement).tagName) return false;
      const el = target as HTMLElement;
      const tag = el.tagName;
      if (tag === "INPUT" || tag === "TEXTAREA" || tag === "SELECT") return true;
      if (el.isContentEditable) return true;
      return false;
    };
    const handleGoto = (dest: "team" | "meta") => {
      if (dest === "meta") {
        setShowTeamHeaders((v) => !v);
        return;
      }
      setShowTeamChat(true);
      const el = document.getElementById("team-chat");
      if (el) {
        el.scrollIntoView({ behavior: "smooth", block: "start" });
      }
    };
    const onKey = (event: KeyboardEvent) => {
      if (shouldIgnoreTarget(event.target)) return;
      if (event.key.toLowerCase() === "f" && (event.metaKey || event.ctrlKey)) {
        event.preventDefault();
        teamSearchRef.current?.focus();
        return;
      }
      const now = Date.now();
      const key = event.key.toLowerCase();
      if (key === "g") {
        lastKeyRef.key = "g";
        lastKeyRef.ts = now;
        return;
      }
      if (lastKeyRef.key === "g" && now - lastKeyRef.ts < 800) {
        if (key === "t") {
          event.preventDefault();
          handleGoto("team");
        } else if (key === "m") {
          event.preventDefault();
          handleGoto("meta");
        }
      }
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [setShowTeamChat, setShowTeamHeaders]);

  const conversationItems = React.useMemo(() => {
    const items: {
      kind: "message" | "tool_record";
      ts: number;
      message?: any;
      run?: any;
      runId?: number;
      details?: any;
      toolRecord?: any;
    }[] = [];
    for (const m of dbMessages) {
      const ts = typeof m?.created_unix_ms === "number" ? m.created_unix_ms : 0;
      if (!ts) continue;
      items.push({ kind: "message", ts, message: m });
    }
    for (const r of dbRuns) {
      const ts = typeof r?.ts_unix_ms === "number" ? r.ts_unix_ms : 0;
      if (!ts) continue;
      const runId = typeof r?.run_id === "number" ? r.run_id : Number(r?.run_id ?? r?.id ?? NaN);
      const details = Number.isFinite(runId) ? dbRunDetailsById[runId] : undefined;
      const safeRunId = Number.isFinite(runId) ? runId : undefined;
      const toolRecords = details && Array.isArray(details?.tool_records) ? (details.tool_records as any[]) : [];
      toolRecords.forEach((tr, idx) => {
        const toolTs =
          typeof tr?.ts_unix_ms === "number"
            ? tr.ts_unix_ms
            : typeof tr?.created_unix_ms === "number"
              ? tr.created_unix_ms
              : typeof tr?.updated_unix_ms === "number"
                ? tr.updated_unix_ms
                : ts + idx + 1;
        items.push({
          kind: "tool_record",
          ts: toolTs,
          run: r,
          runId: safeRunId,
          details,
          toolRecord: tr,
        });
      });
    }
    items.sort((a, b) => a.ts - b.ts);
    return items;
  }, [dbMessages, dbRuns, dbRunDetailsById]);

  const formatJson = (raw: string): string => {
    const txt = String(raw ?? "");
    if (!txt) return "";
    try {
      const parsed = JSON.parse(txt);
      return JSON.stringify(parsed, null, 2);
    } catch {
      return txt;
    }
  };

  const lastAssistantMessage = React.useMemo(() => {
    let last: any = null;
    for (const m of dbMessages) {
      if (!m || typeof m !== "object") continue;
      if (m.role !== "assistant") continue;
      const ts = typeof m.created_unix_ms === "number" ? m.created_unix_ms : 0;
      if (!last || ts >= (typeof last.created_unix_ms === "number" ? last.created_unix_ms : 0)) {
        last = m;
      }
    }
    return last;
  }, [dbMessages]);

  const lastRun = React.useMemo(() => {
    let last: any = null;
    for (const r of dbRuns) {
      if (!r || typeof r !== "object") continue;
      const ts = typeof r.ts_unix_ms === "number" ? r.ts_unix_ms : 0;
      if (!last || ts >= (typeof last.ts_unix_ms === "number" ? last.ts_unix_ms : 0)) {
        last = r;
      }
    }
    return last;
  }, [dbRuns]);

  const entryKeyFor = React.useCallback((entry: any) => {
    const isLive = entry?.live === true;
    const jobId = typeof entry?.job_id === "string" ? entry.job_id : "";
    const ts = typeof entry?.ts_unix_ms === "number" ? entry.ts_unix_ms : 0;
    return isLive && jobId ? `job:${jobId}` : `ts:${String(ts || 0)}`;
  }, []);

  const expandedKeyCandidates = React.useMemo(() => {
    const keys: string[] = [];
    for (const e of entries) {
      keys.push(entryKeyFor(e));
      if (keys.length >= MAX_HISTORY_EXPANDED_KEYS) break;
    }
    return keys;
  }, [entries, entryKeyFor]);

  React.useEffect(() => {
    if (expandedKeyCandidates.length === 0) return;
    const keep = new Set(expandedKeyCandidates);
    props.setHistoryExpandedByKey((prev) => {
      const cur = prev || {};
      let changed = false;
      const next: Record<string, boolean> = {};
      for (const key of Object.keys(cur)) {
        if (keep.has(key)) next[key] = cur[key];
        else changed = true;
      }
      return changed ? next : cur;
    });
  }, [expandedKeyCandidates, props.setHistoryExpandedByKey]);

  return (
    <div>
      <div className="mb-4">
        {lastAssistantMessage || lastRun ? (
          <div className="mb-4 rounded-lg border border-white/10 bg-white/5 px-3 py-2">
            <div className="text-xs font-semibold text-white/70">Latest outcome</div>
            {lastAssistantMessage && typeof lastAssistantMessage.content === "string" ? (
              <details className="mt-2">
                <summary className="cursor-pointer text-[11px] text-white/60">Assistant response (collapsed)</summary>
                <div className="mt-2 text-sm text-white/90">
                  <Markdown text={String(lastAssistantMessage.content)} />
                </div>
              </details>
            ) : null}
            {lastRun ? (
              <details className="mt-2">
                <summary className="cursor-pointer text-[11px] text-white/60">Run info (collapsed)</summary>
                <div className="mt-2 flex flex-wrap items-center gap-2 text-[11px] text-white/60">
                  <span className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 font-semibold text-white/80">
                    run
                  </span>
                  <span className="text-white/50">
                    {typeof lastRun.ts_unix_ms === "number" ? new Date(lastRun.ts_unix_ms).toLocaleString() : ""}
                  </span>
                  {typeof lastRun.ok === "number" ? (
                    <span className={lastRun.ok === 1 ? "text-emerald-300" : "text-rose-300"}>
                      {lastRun.ok === 1 ? "ok" : "error"}
                    </span>
                  ) : null}
                  {lastRun.error ? <span className="text-rose-200">{String(lastRun.error)}</span> : null}
                </div>
              </details>
            ) : null}
          </div>
        ) : null}

        {teamId ? (
          <div id="team-chat" className="mb-4 rounded-lg border border-white/10 bg-white/5 px-3 py-2">
            <div className="flex items-center justify-between gap-2">
              <div className="text-sm font-semibold text-white/80">Team chat</div>
              <div className="flex items-center gap-2">
                <button
                  className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                  type="button"
                  onClick={() => setShowTeamHeaders((v) => !v)}
                >
                  {showTeamHeaders ? "Hide meta" : "Show meta"}
                </button>
                <button
                  className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                  type="button"
                  onClick={() => setShowTeamChat((v) => !v)}
                >
                  {showTeamChat ? "Hide" : "Show"}
                </button>
              </div>
            </div>
            {showTeamChat ? (
              <div className="mt-2 flex flex-wrap items-center gap-2 text-[11px] text-white/50">
                <input
                  className="min-w-[180px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80"
                  value={teamSearch}
                  onChange={(e) => setTeamSearch(e.target.value)}
                  placeholder="Filter team chat…"
                  ref={teamSearchRef}
                  onKeyDown={(event) => {
                    if (event.key === "Escape") {
                      setTeamSearch("");
                    }
                  }}
                />
                {teamSearch.trim().length > 0 ? (
                  <button
                    className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                    type="button"
                    onClick={() => setTeamSearch("")}
                  >
                    Clear
                  </button>
                ) : null}
              </div>
            ) : null}
            <div className="mt-1 text-[10px] text-white/40">Shortcuts: g t (jump), g m (toggle meta)</div>
            <div className="mt-1 flex flex-wrap items-center gap-2 text-[11px] text-white/50">
              <span className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-white/70">
                team {teamId}
              </span>
              {teamRunId ? (
                <span className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-white/60">
                  run {teamRunId}
                </span>
              ) : null}
              {teamRunStatus ? (
                <span className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-white/60">
                  {teamRunStatus}
                </span>
              ) : null}
              {teamRunCreatedMs ? (
                <span className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-white/50">
                  started {new Date(teamRunCreatedMs).toLocaleString()}
                </span>
              ) : null}
              {teamAgentSummary.length > 0 ? (
                <span className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-white/50">
                  {teamAgentSummary.length} agents active
                </span>
              ) : null}
              {teamLastActivity ? (
                <span className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-white/50">
                  last activity {new Date(teamLastActivity).toLocaleString()}
                </span>
              ) : null}
            </div>
            <div className="mt-2 grid gap-2 sm:grid-cols-2">
              <div className="rounded-md border border-white/10 bg-black/20 px-2 py-1">
                <div className="text-[10px] uppercase tracking-wide text-white/40">Run status</div>
                <div className="mt-1 text-[11px] text-white/70">
                  {teamRunId ? (
                    <>
                      <span className="text-white/80">Run {teamRunId}</span>
                      {teamRunStatus ? <span className="text-white/50"> · {teamRunStatus}</span> : null}
                      {teamRunCreatedMs ? (
                        <span className="text-white/40"> · {new Date(teamRunCreatedMs).toLocaleString()}</span>
                      ) : null}
                    </>
                  ) : (
                    <span className="text-white/50">No active run</span>
                  )}
                </div>
              </div>
              <div className="rounded-md border border-white/10 bg-black/20 px-2 py-1">
                <div className="text-[10px] uppercase tracking-wide text-white/40">Agents</div>
                <div className="mt-1 text-[11px] text-white/70">
                  {teamAgentSummary.length > 0 ? `${teamAgentSummary.length} active` : "No agent activity yet"}
                </div>
              </div>
            </div>
            {teamAgentSummary.length > 0 ? (
              <div className="mt-2 grid gap-2 md:grid-cols-2">
                {teamAgentSummary.slice(0, showAllTeamAgents ? teamAgentSummary.length : 4).map((agent) => (
                  <div key={agent.agentId} className="rounded-md border border-white/10 bg-black/20 px-2 py-1 text-[11px] text-white/70">
                    <div className="flex items-center justify-between gap-2">
                      <span className="text-white/80">{agent.agentId}</span>
                      <span className="text-white/40">
                        {agent.lastTs ? new Date(agent.lastTs).toLocaleString() : "unknown"}
                      </span>
                    </div>
                    <div className="mt-1 text-white/50">
                      {agent.lastContent.trim().length > 0 ? agent.lastContent.trim().slice(0, 120) : "(no content)"}
                    </div>
                  </div>
                ))}
                {teamAgentSummary.length > 4 ? (
                  <button
                    className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                    type="button"
                    onClick={() => setShowAllTeamAgents((v) => !v)}
                  >
                    {showAllTeamAgents ? "Show fewer agents" : `Show all ${teamAgentSummary.length} agents`}
                  </button>
                ) : null}
              </div>
            ) : null}
            <details
              className="mt-2 rounded-md border border-white/10 bg-black/20 p-2 text-[11px] text-white/50"
              open={!teamFiltersCollapsed}
              onToggle={(event) => {
                const open = (event.currentTarget as HTMLDetailsElement).open;
                setTeamFiltersCollapsed(!open);
              }}
            >
              <summary className="cursor-pointer select-none text-[11px] text-white/70">
                <div className="flex flex-wrap items-center gap-2">
                  <span className="font-semibold text-white/80">Filters & controls</span>
                  {filtersActive ? (
                    <span className="rounded-md border border-amber-400/30 bg-amber-500/10 px-2 py-0.5 text-[11px] text-amber-100">
                      active
                    </span>
                  ) : null}
                  {teamSearch.trim().length > 0 ? <span className="text-white/50">“{teamSearch.trim()}”</span> : null}
                  {teamSearch.trim().length > 0 ? (
                    <span className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-[11px] text-white/70">
                      {teamFilteredCount} match{teamFilteredCount === 1 ? "" : "es"}
                    </span>
                  ) : null}
                </div>
              </summary>
              <div className="mt-2 flex flex-wrap items-center gap-2 text-[11px] text-white/50">
                {teamSavedFilters.length > 0 ? (
                  <select
                    className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80"
                    value=""
                    onChange={(e) => {
                      const val = e.target.value;
                      if (val) setTeamSearch(val);
                    }}
                  >
                    <option value="">Saved filters…</option>
                    {teamSavedFilters.map((f) => (
                      <option key={f} value={f}>
                        {f}
                      </option>
                    ))}
                  </select>
                ) : null}
                {teamSearch.trim().length > 0 ? (
                  <button
                    className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                    type="button"
                    onClick={() => {
                      const val = teamSearch.trim();
                      if (!val) return;
                      setTeamSavedFilters((prev) => (prev.includes(val) ? prev : [...prev, val]));
                    }}
                  >
                    Save filter
                  </button>
                ) : null}
                {teamSearch.trim().length > 0 && !teamPinnedFilters.includes(teamSearch.trim()) ? (
                  <button
                    className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                    type="button"
                    onClick={() => {
                      const val = teamSearch.trim();
                      if (!val) return;
                      setTeamPinnedFilters((prev) => (prev.includes(val) ? prev : [...prev, val]));
                    }}
                  >
                    Pin filter
                  </button>
                ) : null}
                {teamSavedFilters.length > 0 ? (
                  <details className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70">
                    <summary className="cursor-pointer select-none">Manage saved</summary>
                    <div className="mt-2 grid gap-1">
                      {teamSavedFilters.map((f) => (
                        <div key={`manage-${f}`} className="flex items-center gap-2">
                          <button
                            className="flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-left text-[11px] text-white/70 hover:bg-black/40"
                            type="button"
                            onClick={() => setTeamSearch(f)}
                          >
                            {f}
                          </button>
                          <button
                            className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-[11px] text-white/70 hover:bg-black/40"
                            type="button"
                            onClick={() =>
                              setTeamPinnedFilters((prev) =>
                                prev.includes(f) ? prev.filter((x) => x !== f) : [...prev, f],
                              )
                            }
                          >
                            {teamPinnedFilters.includes(f) ? "Unpin" : "Pin"}
                          </button>
                          <button
                            className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-[11px] text-white/70 hover:bg-black/40"
                            type="button"
                            onClick={() => setTeamSavedFilters((prev) => prev.filter((x) => x !== f))}
                          >
                            Remove
                          </button>
                        </div>
                      ))}
                      <button
                        className="rounded-md border border-rose-400/30 bg-rose-500/10 px-2 py-0.5 text-left text-[11px] text-rose-100 hover:bg-rose-500/20"
                        type="button"
                        onClick={() => setTeamSavedFilters([])}
                      >
                        Clear all saved filters
                      </button>
                    </div>
                  </details>
                ) : null}
                {teamSearch.trim().length === 0 ? (
                  <span className="text-[11px] text-white/40">Try “executor”, an agent id, or a keyword</span>
                ) : null}
                {teamPinnedFilters.length > 0 ? (
                  <div className="flex flex-wrap items-center gap-1">
                    {teamPinnedFilters.map((chip) => (
                      <div key={`pin-${chip}`} className="flex items-center gap-1">
                        <button
                          className="rounded-md border border-emerald-400/30 bg-emerald-500/10 px-2 py-0.5 text-[11px] text-emerald-100 hover:bg-emerald-500/20"
                          type="button"
                          onClick={() => setTeamSearch(chip)}
                        >
                          {chip}
                        </button>
                        <button
                          className="rounded-md border border-white/10 bg-black/30 px-1.5 py-0.5 text-[11px] text-white/60 hover:bg-black/40"
                          type="button"
                          onClick={() => setTeamPinnedFilters((prev) => prev.filter((x) => x !== chip))}
                          title="Unpin"
                        >
                          ×
                        </button>
                      </div>
                    ))}
                  </div>
                ) : null}
                <div className="flex flex-wrap items-center gap-1">
                  {["user", "assistant", "tool"].map((chip) => (
                    <button
                      key={chip}
                      className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-[11px] text-white/70 hover:bg-black/40"
                      type="button"
                      onClick={() => setTeamSearch((prev) => (prev === chip ? "" : chip))}
                    >
                      {chip}
                    </button>
                  ))}
                  {teamRoleChips.map((chip) => (
                    <button
                      key={`role:${chip}`}
                      className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-[11px] text-white/70 hover:bg-black/40"
                      type="button"
                      onClick={() => setTeamSearch((prev) => (prev === chip ? "" : chip))}
                    >
                      {chip}
                    </button>
                  ))}
                </div>
                <button
                  className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                  type="button"
                  onClick={() => setShowTeamRunMarkers((v) => !v)}
                >
                  {showTeamRunMarkers ? "Hide run markers" : "Show run markers"}
                </button>
                <button
                  className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                  type="button"
                  onClick={() => setShowTeamGroupByAgent((v) => !v)}
                >
                  {showTeamGroupByAgent ? "Timeline view" : "Group by agent"}
                </button>
                <button
                  className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                  type="button"
                  onClick={() => setShowTeamRoleLabels((v) => !v)}
                >
                  {showTeamRoleLabels ? "Hide role labels" : "Show role labels"}
                </button>
                <button
                  className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                  type="button"
                  onClick={() => setShowTeamSystemMessages((v) => !v)}
                >
                  {showTeamSystemMessages ? "Hide system" : "Show system"}
                </button>
                {teamMutedAgents.length > 0 ? (
                  <button
                    className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                    type="button"
                    onClick={() => setTeamMutedAgents([])}
                  >
                    Clear muted
                  </button>
                ) : null}
                {teamSearch.trim().length > 0 ? (
                  <button
                    className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                    type="button"
                    onClick={() => setTeamSearch("")}
                  >
                    Clear filter
                  </button>
                ) : null}
                {teamSearch.trim().length > 0 ? (
                  <>
                    <button
                      className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                      type="button"
                      onClick={() => jumpToMatch("first")}
                    >
                      First match
                    </button>
                    <button
                      className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                      type="button"
                      onClick={() => jumpToMatch("last")}
                    >
                      Last match
                    </button>
                  </>
                ) : null}
                {filtersActive ? (
                  <button
                    className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                    type="button"
                    onClick={() => {
                      setTeamSearch("");
                      setTeamMutedAgents([]);
                      setShowTeamSystemMessages(false);
                      setShowTeamRunMarkers(true);
                    }}
                  >
                    Clear all
                  </button>
                ) : null}
                <button
                  className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                  type="button"
                  onClick={() => {
                    if (typeof window !== "undefined") {
                      window.scrollTo({ top: document.body.scrollHeight, behavior: "smooth" });
                    }
                  }}
                >
                  Jump to latest
                </button>
                {showTeamGroupByAgent && teamGroupedByAgent.length > 0 ? (
                  <button
                    className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                    type="button"
                    onClick={() => setTeamGroupState({})}
                  >
                    Collapse groups
                  </button>
                ) : null}
              </div>
            </details>
            {teamMutedAgents.length > 0 ? (
              <div className="mt-2 flex flex-wrap items-center gap-2 text-[11px] text-white/50">
                Muted:
                {teamMutedAgents.map((agent) => (
                  <button
                    key={agent}
                    className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-[11px] text-white/70 hover:bg-black/40"
                    type="button"
                    onClick={() => setTeamMutedAgents((prev) => prev.filter((id) => id !== agent))}
                  >
                    {agent} ×
                  </button>
                ))}
              </div>
            ) : null}
            {teamConversationWarnings.length > 0 ? (
              <div className="mt-2 text-[11px] text-amber-200">{teamConversationWarnings[0]}</div>
            ) : null}
            {showTeamChat ? (
              teamConversationItems.length === 0 ? (
                <div className="mt-3 rounded-md border border-white/10 bg-black/20 px-3 py-3 text-xs text-white/60">
                  No team messages yet. Start a team run to populate this chat.
                </div>
              ) : (
                <div className="mt-3 grid gap-3">
                  {teamSearch.trim().length > 0 && teamFilteredCount === 0 ? (
                    <div className="rounded-md border border-amber-400/30 bg-amber-500/10 px-3 py-2 text-[11px] text-amber-100">
                      No matches.{" "}
                      <button
                        className="underline hover:text-white"
                        type="button"
                        onClick={() => setTeamSearch("")}
                      >
                        Clear filter
                      </button>
                      .
                    </div>
                  ) : null}
                  {showTeamGroupByAgent
                    ? teamGroupedByAgent.map((group) => {
                        const isMuted = mutedAgentSet.has(group.key);
                        const isOpen =
                          Object.prototype.hasOwnProperty.call(teamGroupState, group.key)
                            ? !!teamGroupState[group.key]
                            : false;
                        return (
                          <details
                            key={group.key}
                            className="rounded-md border border-white/10 bg-black/20 p-2"
                            open={isOpen}
                            onToggle={(event) => {
                              const open = (event.currentTarget as HTMLDetailsElement).open;
                              setTeamGroupState((prev) => ({ ...(prev || {}), [group.key]: open }));
                            }}
                          >
                            <summary className="cursor-pointer text-[11px] text-white/70">
                              <div className="flex flex-wrap items-center justify-between gap-2">
                                <div className="flex flex-wrap items-center gap-2">
                                  <span className="text-white/80">{group.label}</span>
                                  <span className="text-white/40">· {group.items.length} messages</span>
                                  <span className="text-white/40">
                                    · last {group.latest ? new Date(group.latest).toLocaleString() : "unknown"}
                                  </span>
                                </div>
                                <button
                                  className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-[11px] text-white/70 hover:bg-black/40"
                                  type="button"
                                  onClick={(event) => {
                                    event.preventDefault();
                                    event.stopPropagation();
                                    setTeamMutedAgents((prev) =>
                                      isMuted ? prev.filter((id) => id !== group.key) : [...prev, group.key],
                                    );
                                  }}
                                >
                                  {isMuted ? "Unmute" : "Mute"}
                                </button>
                              </div>
                              <div className="mt-1 text-[11px] text-white/50">{group.preview}</div>
                            </summary>
                            <div className="mt-2 grid gap-2">
                              {group.items.map((item, idx) => renderTeamMessage(item, idx))}
                            </div>
                          </details>
                        );
                      })
                    : teamTimelineItems.map((entry, idx) => {
                        if (entry.kind === "marker") {
                          return (
                            <div
                              key={`team-run:${entry.runId || idx}`}
                              className="rounded-md border border-indigo-400/20 bg-indigo-500/10 px-3 py-2 text-[11px] text-indigo-100"
                            >
                              Run {entry.runId}
                            </div>
                          );
                        }
                        return renderTeamMessage(entry.item, idx);
                      })}
                </div>
              )
            ) : null}
          </div>
        ) : null}
        <div className="mb-2 flex items-center justify-between gap-2">
          <div className="text-sm font-semibold text-white/80">Conversation</div>
          <div className="text-[11px] text-white/50">
            {conversationItems.length > 0 ? `${conversationItems.length} items` : "no persisted history"}
          </div>
        </div>
        {conversationItems.length === 0 ? (
          <div className="rounded-md border border-white/10 bg-black/20 px-3 py-3 text-xs text-white/60">
            No persisted messages yet. Run a prompt to populate the chat history.
          </div>
        ) : (
          <div className="grid gap-3">
            {conversationItems.map((item, idx) => {
              if (item.kind === "message") {
                const m = item.message ?? {};
                const role = typeof m?.role === "string" ? m.role : "message";
                const content = typeof m?.content === "string" ? m.content : "";
                const ts = typeof m?.created_unix_ms === "number" ? m.created_unix_ms : 0;
                const when = ts ? new Date(ts).toLocaleString() : "";
                const truncated = m?.content_truncated ? true : false;
                const mmJson = typeof m?.mm_json === "string" ? m.mm_json : "";
                const mmBytes = typeof m?.mm_bytes === "number" ? m.mm_bytes : mmJson.length || 0;
                const isSystem = role === "system";
                const hasMeta = !!truncated || mmBytes > 0;
                return (
                  <div key={`msg:${ts || idx}`} className="rounded-lg border border-white/10 bg-white/5 px-3 py-2">
                    <div className="flex flex-wrap items-center gap-2 text-[11px] text-white/60">
                      <span className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 font-semibold text-white/80">
                        {role}
                      </span>
                      {when ? <span>{when}</span> : null}
                      {truncated ? <span className="text-amber-200">truncated</span> : null}
                    </div>
                    {isSystem ? (
                      <details className="mt-2">
                        <summary className="cursor-pointer text-xs text-white/60">System prompt (collapsed)</summary>
                        <div className="mt-2 text-sm text-white/90">
                          {content ? <Markdown text={content} /> : <span className="text-white/50">(no content)</span>}
                        </div>
                      </details>
                    ) : (
                      <div className="mt-2 text-sm text-white/90">
                        {content ? <Markdown text={content} /> : <span className="text-white/50">(no content)</span>}
                      </div>
                    )}
                    {hasMeta ? (
                      <details className="mt-2">
                        <summary className="cursor-pointer text-[11px] text-white/60">Message details</summary>
                        <div className="mt-2 text-[11px] text-white/60">
                          {mmBytes > 0 ? <div>mm bytes: {mmBytes}</div> : null}
                          {mmJson ? (
                            <details className="mt-2">
                              <summary className="cursor-pointer text-[11px] text-white/60">mm json</summary>
                              <pre className="mt-2 overflow-auto whitespace-pre-wrap break-words rounded-md border border-white/10 bg-black/30 p-2 text-[11px] leading-relaxed text-white/90">
                                {formatJson(mmJson)}
                              </pre>
                            </details>
                          ) : null}
                        </div>
                      </details>
                    ) : null}
                  </div>
                );
              }

              if (item.kind === "tool_record") {
                const tr = item.toolRecord ?? {};
                const toolName = typeof tr?.tool_name === "string" ? tr.tool_name : "tool";
                const argsJson = typeof tr?.arguments_json === "string" ? tr.arguments_json : "";
                let parsedArgs: any = null;
                try {
                  parsedArgs = argsJson ? JSON.parse(argsJson) : null;
                } catch {
                  parsedArgs = null;
                }
                const cmd = typeof parsedArgs?.cmd === "string" ? parsedArgs.cmd : "";
                const argvRaw = Array.isArray(parsedArgs?.argv) ? parsedArgs.argv : null;
                const argv = argvRaw ? argvRaw.map((x: any) => (typeof x === "string" ? x : "")).filter(Boolean).join(" ") : "";
                const command = cmd || argv;
                const resultText = typeof tr?.result_text === "string" ? tr.result_text : "";
                const resultForPrompt = typeof tr?.result_for_prompt_text === "string" ? tr.result_for_prompt_text : "";
                const truncatedForPrompt = tr?.result_truncated_for_prompt ? true : false;
                const shortCommand = command ? (command.length > 140 ? `${command.slice(0, 140)}…` : command) : "";
                return (
                  <div key={`tool:${item.ts || idx}`} className="rounded-lg border border-white/10 bg-white/5 px-3 py-2">
                    <div className="flex flex-wrap items-center gap-2 text-[11px] text-white/60">
                      <span className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 font-semibold text-white/80">
                        tool
                      </span>
                      <span className="text-white/50">{toolName}</span>
                      {shortCommand ? <span className="text-white/40">· {shortCommand}</span> : null}
                    </div>
                    {command ? (
                      <div className="mt-2">
                        <div className="text-[11px] text-white/60">Command</div>
                        <pre className="mt-2 overflow-auto whitespace-pre-wrap break-words rounded-md border border-white/10 bg-black/20 p-2 font-mono text-[11px] leading-relaxed text-white/90">
                          {command}
                        </pre>
                      </div>
                    ) : null}
                    {resultText ? (
                      <details className="mt-2" open>
                        <summary className="cursor-pointer text-[11px] text-white/60">Output</summary>
                        <pre className="mt-2 overflow-auto whitespace-pre-wrap break-words rounded-md border border-white/10 bg-black/30 p-2 text-[11px] leading-relaxed text-white/90">
                          {resultText}
                        </pre>
                      </details>
                    ) : null}
                    {resultForPrompt && resultForPrompt !== resultText ? (
                      <details className="mt-2">
                        <summary className="cursor-pointer text-[11px] text-white/60">
                          Prompt-facing output{truncatedForPrompt ? " (truncated)" : ""}
                        </summary>
                        <pre className="mt-2 overflow-auto whitespace-pre-wrap break-words rounded-md border border-white/10 bg-black/30 p-2 text-[11px] leading-relaxed text-white/90">
                          {resultForPrompt}
                        </pre>
                      </details>
                    ) : null}
                    {argsJson && !command ? (
                      <details className="mt-2" open>
                        <summary className="cursor-pointer text-[11px] text-white/60">Arguments</summary>
                        <pre className="mt-2 overflow-auto whitespace-pre-wrap break-words rounded-md border border-white/10 bg-black/30 p-2 text-[11px] leading-relaxed text-white/90">
                          {formatJson(argsJson)}
                        </pre>
                      </details>
                    ) : argsJson ? (
                      <details className="mt-2">
                        <summary className="cursor-pointer text-[11px] text-white/60">Arguments (collapsed)</summary>
                        <pre className="mt-2 overflow-auto whitespace-pre-wrap break-words rounded-md border border-white/10 bg-black/30 p-2 text-[11px] leading-relaxed text-white/90">
                          {formatJson(argsJson)}
                        </pre>
                      </details>
                    ) : null}
                  </div>
                );
              }

              return null;
            })}
          </div>
        )}
      </div>
      <div className="mb-4">
        <div className="mb-2 flex items-center justify-between gap-2">
          <div className="text-sm font-semibold text-white/80">Artifacts</div>
          <div className="text-[11px] text-white/50">
            {sessionArtifacts.length > 0 ? `${sessionArtifacts.length} items` : "none"}
          </div>
        </div>
        {sessionArtifacts.length === 0 ? (
          <div className="rounded-md border border-white/10 bg-black/20 px-3 py-3 text-xs text-white/60">
            No artifacts captured yet.
          </div>
        ) : (
          <div className="grid gap-2">
            {sessionArtifacts.slice(0, 20).map((row, idx) => {
              const data = row?.data ?? {};
              const artifact = data?.artifact ?? row?.artifact ?? row;
              const key =
                typeof artifact?.path === "string"
                  ? `artifact:${artifact.path}`
                  : typeof artifact?.resolved_path === "string"
                    ? `artifact:${artifact.resolved_path}`
                    : `artifact:${idx}`;
              return (
                <div key={key} className="rounded-md border border-white/10 bg-black/20 p-3">
                  <ArtifactView
                    baseUrl={props.effectiveBase}
                    yolo={props.yolo}
                    artifact={artifact}
                    allowAutoplay={props.allowAutoplay}
                    sessionId={props.sessionId}
                    client={props.client}
                    daemonAuth={props.daemonAuth}
                  />
                </div>
              );
            })}
          </div>
        )}
      </div>
      <div className="mb-2 flex items-center justify-between gap-2">
        <div className="text-sm font-semibold text-white/80">Technical history</div>
        <div className="flex items-center gap-2">
          <button
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
            type="button"
            onClick={() => setShowTechnicalHistory((v) => !v)}
            title={showTechnicalHistory ? "Hide technical history" : "Show technical history"}
          >
            {showTechnicalHistory ? "Hide technical" : "Show technical"}
          </button>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
            type="button"
            onClick={() => props.setShowMessages((v) => !v)}
            title={props.showMessages ? "Hide per-run event transcript" : "Show per-run event transcript"}
          >
            {props.showMessages ? "Hide messages" : "Show messages"}
          </button>
          {entries.length > 1 ? (
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
              type="button"
              onClick={() => props.setShowAllEntries((v) => !v)}
              title={props.showAllEntries ? "Hide older history entries" : "Show older history entries"}
            >
              {props.showAllEntries ? `Hide history (${entries.length - 1})` : `Show history (${entries.length - 1})`}
            </button>
          ) : null}
        </div>
      </div>

      {showTechnicalHistory ? (
        entries.length === 0 ? (
          <div className="rounded-md border border-white/10 bg-black/20 px-3 py-3 text-xs text-white/60">
            No history yet. Run a prompt to populate the timeline.
          </div>
        ) : (
          <div className="grid gap-2">
            {(props.showAllEntries ? entries : entries.slice(0, 1)).map((e: any, idx: number) => {
              const ts = typeof e?.ts_unix_ms === "number" ? e.ts_unix_ms : 0;
              const when = ts ? new Date(ts).toLocaleString() : "";
              const promptText = typeof e?.prompt === "string" ? e.prompt : "";
              const assistantText = typeof e?.assistant_text === "string" ? e.assistant_text : "";
              const evs = Array.isArray(e?.events) ? (e.events as any[]) : [];
              const ok = typeof e?.ok === "boolean" ? e.ok : undefined;
              const isLive = e?.live === true;
              const jobId = typeof e?.job_id === "string" ? e.job_id : "";
              const jobSt = typeof e?.job_status === "string" ? e.job_status : "";
              const traceId = typeof e?.trace_id === "string" ? e.trace_id : "";
              const status = ok === true ? "ok" : ok === false ? "error" : "";
              const summary = promptText.trim().length > 0 ? promptText.trim().slice(0, 200) : "(no prompt)";
              const entryKey = entryKeyFor(e);
              const expanded =
                Object.prototype.hasOwnProperty.call(props.historyExpandedByKey, entryKey)
                  ? !!props.historyExpandedByKey[entryKey]
                  : idx === 0;
              const tools = typeof e?.tools === "string" ? e.tools : "";
              const yolo = typeof e?.yolo === "boolean" ? e.yolo : undefined;
              const hostPolicy = typeof e?.host_policy === "string" ? e.host_policy : "";
              const automationProfile =
                typeof e?.effective_automation_profile === "string"
                  ? e.effective_automation_profile
                  : typeof e?.automation_profile === "string"
                    ? e.automation_profile
                    : "";
              const model = typeof e?.model === "string" ? e.model : "";
              const baseUrl = typeof e?.base_url === "string" ? e.base_url : "";
              const meta = [
                tools ? { label: "tools", value: tools } : null,
                typeof yolo === "boolean" ? { label: "yolo", value: yolo ? "true" : "false" } : null,
                hostPolicy ? { label: "host_policy", value: hostPolicy } : null,
                automationProfile ? { label: "automation_profile", value: automationProfile } : null,
                model ? { label: "model", value: model } : null,
                baseUrl ? { label: "base_url", value: baseUrl } : null,
              ].filter(Boolean) as { label: string; value: string }[];

              return (
                <details
                  key={entryKey}
                  open={expanded}
                  onToggle={(ev) => {
                    const open = (ev.currentTarget as HTMLDetailsElement).open;
                    props.setHistoryExpandedByKey((prev) => ({ ...(prev || {}), [entryKey]: open }));
                  }}
                  className="rounded-lg border border-white/10 bg-white/5 px-3 py-2"
                >
                  <summary className="cursor-pointer select-none text-xs text-white/80">
                    <div className="flex min-w-0 flex-wrap items-baseline gap-x-2 gap-y-1">
                      <span className="shrink-0 text-white/60">{when}</span>
                      {isLive ? (
                        <span className="shrink-0 text-indigo-300">
                          running{jobSt ? ` (${jobSt})` : ""}
                          {jobId ? (
                            <>
                              {" "}
                              <code className="inline-block max-w-[50vw] truncate align-bottom text-indigo-200/80" title={jobId}>
                                {jobId}
                              </code>
                            </>
                          ) : null}
                        </span>
                      ) : null}
                      {status ? (
                        <span className={`${status === "ok" ? "text-emerald-300" : "text-rose-300"}`}>{status}</span>
                      ) : null}
                      {traceId ? (
                        <button
                          className="shrink-0 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
                          type="button"
                          title="Open trace lookup for this run"
                          onClick={(ev) => {
                            ev.preventDefault();
                            ev.stopPropagation();
                            props.onTraceIdClick(traceId);
                          }}
                        >
                          Trace
                        </button>
                      ) : null}
                      <span className="min-w-0 flex-1">{summary}</span>
                      <span className="shrink-0 text-white/40">({evs.length} events)</span>
                    </div>
                  </summary>
                  <div className="mt-3 grid gap-3">
                    {meta.length > 0 ? (
                      <div className="rounded-md border border-white/10 bg-black/20 px-3 py-2 text-[11px] text-white/70">
                        <div className="mb-1 text-[11px] font-semibold text-white/60">Run settings</div>
                        <div className="flex flex-wrap gap-2">
                          {meta.map((item) => (
                            <div
                              key={`${item.label}:${item.value}`}
                              className="flex items-center gap-1 rounded-md border border-white/10 bg-black/30 px-2 py-1"
                            >
                              <span className="text-white/50">{item.label}</span>
                              <span className="font-mono text-white/80">{item.value}</span>
                            </div>
                          ))}
                        </div>
                      </div>
                    ) : null}
                    {assistantText ? (
                      <div className="rounded-md border border-white/10 bg-black/20 px-3 py-2">
                        <div className="flex items-start gap-2">
                          <div className="shrink-0 text-[11px] font-semibold text-white/60">Assistant</div>
                          <div className="min-w-0 flex-1">
                            <Markdown text={assistantText} />
                          </div>
                        </div>
                      </div>
                    ) : null}
                    {evs.length > 0 && (isLive || props.showMessages) ? (
                      <ConversationView
                        baseUrl={props.effectiveBase}
                        yolo={props.yolo}
                        sessionId={props.sessionId}
                        client={props.client}
                        daemonAuth={props.daemonAuth}
                        prompt={promptText}
                        events={evs as any}
                        showDebugEvents={props.showDebugInConversation}
                        allowAutoplay={props.allowAutoplay}
                        allowClientRpcs={props.allowClientRpcs}
                        allowClientEffects={props.allowClientEffects}
                        allowUnsafePageEval={props.allowUnsafePageEval}
                        reverseOrder={false}
                        disableAutoClientRpcs={idx !== 0}
                        sceneEntities={props.sceneEntities}
                        onSceneApply={props.onSceneApply}
                      />
                    ) : null}
                  </div>
                </details>
              );
            })}
          </div>
        )
      ) : (
        <div className="rounded-md border border-white/10 bg-black/20 px-3 py-3 text-xs text-white/50">
          Technical history is hidden by default.
        </div>
      )}
    </div>
  );
}
