import React from "react";
import Markdown from "../Markdown";
import useLocalStorageState from "../../hooks/useLocalStorageState";

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

type UseHistoryPanelTeamStateArgs = {
  effectiveBase: string;
  sessionId: string;
  teamId: string;
  teamConversationItems: any[];
};

export default function useHistoryPanelTeamState(args: UseHistoryPanelTeamStateArgs) {
  const { effectiveBase, sessionId, teamId, teamConversationItems } = args;
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
    const base = String(effectiveBase || "").trim() || "default";
    const tid = teamId || "none";
    return `agentui.teamSummaryExpanded:${base}::${tid}`;
  }, [effectiveBase, teamId]);
  const [showAllTeamAgents, setShowAllTeamAgents] = useLocalStorageState<boolean>(teamSummaryKey, false);

  const teamGroupStateKey = React.useMemo(() => {
    const base = String(effectiveBase || "").trim() || "default";
    const tid = teamId || "none";
    return `agentui.teamGroupState:${base}::${tid}`;
  }, [effectiveBase, teamId]);
  const [teamGroupState, setTeamGroupState] = useLocalStorageState<Record<string, boolean>>(teamGroupStateKey, {});

  const teamFiltersCollapsedKey = React.useMemo(() => {
    const base = String(effectiveBase || "").trim() || "default";
    const tid = teamId || "none";
    return `agentui.teamFiltersCollapsed:${base}::${tid}`;
  }, [effectiveBase, teamId]);
  const [teamFiltersCollapsed, setTeamFiltersCollapsed] = useLocalStorageState<boolean>(teamFiltersCollapsedKey, false);

  const teamChatKey = React.useMemo(() => {
    const base = String(effectiveBase || "").trim() || "default";
    const tid = teamId || "none";
    return `agentui.teamChat:${base}::${tid}`;
  }, [effectiveBase, teamId]);
  const [showTeamChat, setShowTeamChat] = useLocalStorageState<boolean>(teamChatKey, true);

  const teamRunMarkersKey = React.useMemo(() => {
    const base = String(effectiveBase || "").trim() || "default";
    const tid = teamId || "none";
    return `agentui.teamRunMarkers:${base}::${tid}`;
  }, [effectiveBase, teamId]);
  const [showTeamRunMarkers, setShowTeamRunMarkers] = useLocalStorageState<boolean>(teamRunMarkersKey, true);

  const teamGroupByAgentKey = React.useMemo(() => {
    const base = String(effectiveBase || "").trim() || "default";
    const tid = teamId || "none";
    return `agentui.teamGroupByAgent:${base}::${tid}`;
  }, [effectiveBase, teamId]);
  const [showTeamGroupByAgent, setShowTeamGroupByAgent] = useLocalStorageState<boolean>(teamGroupByAgentKey, false);

  const teamRoleLabelsKey = React.useMemo(() => {
    const base = String(effectiveBase || "").trim() || "default";
    const tid = teamId || "none";
    return `agentui.teamRoleLabels:${base}::${tid}`;
  }, [effectiveBase, teamId]);
  const [showTeamRoleLabels, setShowTeamRoleLabels] = useLocalStorageState<boolean>(teamRoleLabelsKey, true);

  const teamHeadersKey = React.useMemo(() => {
    const base = String(effectiveBase || "").trim() || "default";
    const tid = teamId || "none";
    return `agentui.teamHeaders:${base}::${tid}`;
  }, [effectiveBase, teamId]);
  const [showTeamHeaders, setShowTeamHeaders] = useLocalStorageState<boolean>(teamHeadersKey, false);

  const teamSystemKey = React.useMemo(() => {
    const base = String(effectiveBase || "").trim() || "default";
    const tid = teamId || "none";
    return `agentui.teamSystemMessages:${base}::${tid}`;
  }, [effectiveBase, teamId]);
  const [showTeamSystemMessages, setShowTeamSystemMessages] = useLocalStorageState<boolean>(teamSystemKey, false);

  const teamQuietKey = React.useMemo(() => {
    const base = String(effectiveBase || "").trim() || "default";
    const tid = teamId || "none";
    return `agentui.teamQuiet:${base}::${tid}`;
  }, [effectiveBase, teamId]);
  const [teamQuietMode, setTeamQuietMode] = useLocalStorageState<boolean>(teamQuietKey, false);

  const teamHideToolsKey = React.useMemo(() => {
    const base = String(effectiveBase || "").trim() || "default";
    const tid = teamId || "none";
    return `agentui.teamHideTools:${base}::${tid}`;
  }, [effectiveBase, teamId]);
  const [teamHideTools, setTeamHideTools] = useLocalStorageState<boolean>(teamHideToolsKey, false);

  const teamMutedAgentsKey = React.useMemo(() => {
    const base = String(effectiveBase || "").trim() || "default";
    const tid = teamId || "none";
    return `agentui.teamMutedAgents:${base}::${tid}`;
  }, [effectiveBase, teamId]);
  const [teamMutedAgents, setTeamMutedAgents] = useLocalStorageState<string[]>(teamMutedAgentsKey, []);
  const mutedAgentSet = React.useMemo(() => new Set(teamMutedAgents || []), [teamMutedAgents]);

  const teamSearchKey = React.useMemo(() => {
    const base = String(effectiveBase || "").trim() || "default";
    const tid = teamId || "none";
    return `agentui.teamSearch:${base}::${tid}`;
  }, [effectiveBase, teamId]);
  const [teamSearch, setTeamSearch] = useLocalStorageState<string>(teamSearchKey, "");

  const teamDefaultFilterKey = React.useMemo(() => {
    const base = String(effectiveBase || "").trim() || "default";
    const tid = teamId || "none";
    return `agentui.teamDefaultFilter:${base}::${tid}`;
  }, [effectiveBase, teamId]);
  const [teamDefaultFilter, setTeamDefaultFilter] = useLocalStorageState<string>(teamDefaultFilterKey, "");

  const teamSavedFiltersKey = React.useMemo(() => {
    const base = String(effectiveBase || "").trim() || "default";
    const tid = teamId || "none";
    return `agentui.teamSavedFilters:${base}::${tid}`;
  }, [effectiveBase, teamId]);
  const [teamSavedFilters, setTeamSavedFilters] = useLocalStorageState<string[]>(teamSavedFiltersKey, []);

  const teamPinnedFiltersKey = React.useMemo(() => {
    const base = String(effectiveBase || "").trim() || "default";
    const tid = teamId || "none";
    return `agentui.teamPinnedFilters:${base}::${tid}`;
  }, [effectiveBase, teamId]);
  const [teamPinnedFilters, setTeamPinnedFilters] = useLocalStorageState<string[]>(teamPinnedFiltersKey, []);

  const teamCompactKey = React.useMemo(() => {
    const base = String(effectiveBase || "").trim() || "default";
    const tid = teamId || "none";
    return `agentui.teamCompact:${base}::${tid}`;
  }, [effectiveBase, teamId]);
  const [teamCompactMode, setTeamCompactMode] = useLocalStorageState<boolean>(teamCompactKey, false);

  const teamAutoScrollKey = React.useMemo(() => {
    const base = String(effectiveBase || "").trim() || "default";
    const tid = teamId || "none";
    return `agentui.teamAutoScroll:${base}::${tid}`;
  }, [effectiveBase, teamId]);
  const [teamAutoScroll, setTeamAutoScroll] = useLocalStorageState<boolean>(teamAutoScrollKey, true);

  const teamPauseKey = React.useMemo(() => {
    const base = String(effectiveBase || "").trim() || "default";
    const tid = teamId || "none";
    return `agentui.teamPause:${base}::${tid}`;
  }, [effectiveBase, teamId]);
  const [teamPauseUpdates, setTeamPauseUpdates] = useLocalStorageState<boolean>(teamPauseKey, false);

  const teamPausedItemsRef = React.useRef<any[] | null>(null);
  const teamSearchRef = React.useRef<HTMLInputElement | null>(null);
  const lastAutoScrollTsRef = React.useRef<number>(0);

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

  const scrollToTeamMessage = React.useCallback((ts: number) => {
    if (!ts) return;
    const el = document.getElementById(`team-msg-${ts}`);
    if (el) {
      el.scrollIntoView({ behavior: "smooth", block: "center" });
    }
  }, []);

  const openFilters = React.useCallback(() => {
    setTeamFiltersCollapsed(false);
    const el = document.getElementById("team-filters");
    if (el) {
      el.scrollIntoView({ behavior: "smooth", block: "center" });
    }
  }, [setTeamFiltersCollapsed]);

  const jumpToTeamChat = React.useCallback(() => {
    setShowTeamChat(true);
    const el = document.getElementById("team-chat");
    if (el) {
      el.scrollIntoView({ behavior: "smooth", block: "start" });
    }
  }, [setShowTeamChat]);

  const teamTimelineItems = React.useMemo(() => {
    const needle = String(teamSearch || "").trim().toLowerCase();
    const items = teamConversationItems
      .slice()
      .filter((item) => {
        const role = typeof item?.message?.role === "string" ? item.message.role : "";
        if (role === "system" && !showTeamSystemMessages) return false;
        if (teamHideTools && role === "tool") return false;
        if (teamQuietMode && (role === "system" || role === "tool")) return false;
        const agentLabel = typeof item?.meta?.agent_id === "string" ? item.meta.agent_id : "";
        if (agentLabel && mutedAgentSet.has(agentLabel)) return false;
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
  }, [
    mutedAgentSet,
    showTeamRunMarkers,
    showTeamSystemMessages,
    teamConversationItems,
    teamHideTools,
    teamQuietMode,
    teamSearch,
  ]);

  const teamFilteredItems = React.useMemo(() => {
    return teamTimelineItems.filter((entry) => entry.kind === "item");
  }, [teamTimelineItems]);
  const teamFilteredCount = teamFilteredItems.length;

  React.useEffect(() => {
    if (!teamPauseUpdates) {
      teamPausedItemsRef.current = teamFilteredItems;
      return;
    }
    if (!teamPausedItemsRef.current) {
      teamPausedItemsRef.current = teamFilteredItems;
    }
  }, [teamFilteredItems, teamPauseUpdates]);

  const teamVisibleItems = React.useMemo(() => {
    if (!teamPauseUpdates) return teamFilteredItems;
    return teamPausedItemsRef.current || teamFilteredItems;
  }, [teamFilteredItems, teamPauseUpdates]);

  const teamPendingCount = React.useMemo(() => {
    if (!teamPauseUpdates) return 0;
    const previous = teamPausedItemsRef.current;
    if (!previous) return 0;
    return Math.max(0, teamFilteredItems.length - previous.length);
  }, [teamFilteredItems, teamPauseUpdates]);

  const teamGroupedByAgent = React.useMemo(() => {
    if (!showTeamGroupByAgent) {
      return [] as Array<{ key: string; label: string; items: any[]; latest: number; preview: string }>;
    }
    const groups = new Map<string, { label: string; items: any[]; latest: number }>();
    for (const item of teamVisibleItems) {
      const meta = item?.meta ?? {};
      const agentLabel = typeof meta?.agent_id === "string" && meta.agent_id ? meta.agent_id : "";
      const roleLabel = typeof meta?.role === "string" && meta.role ? meta.role : "";
      const baseLabel = agentLabel || roleLabel || "unknown";
      const label = agentLabel && roleLabel ? `${agentLabel} · ${roleLabel}` : baseLabel;
      const ts = typeof item?.ts === "number" ? item.ts : 0;
      if (!groups.has(baseLabel)) {
        groups.set(baseLabel, { label, items: [item], latest: ts });
      } else {
        const group = groups.get(baseLabel)!;
        group.items.push(item);
        if (ts > group.latest) group.latest = ts;
      }
    }
    return Array.from(groups.entries())
      .map(([key, value]) => {
        const sorted = value.items.slice().sort((a, b) => (a?.ts || 0) - (b?.ts || 0));
        const last = sorted[sorted.length - 1];
        const lastContent = typeof last?.message?.content === "string" ? last.message.content : "";
        const preview = lastContent.trim().length > 0 ? lastContent.trim().slice(0, 120) : "(no content)";
        return { key, ...value, items: sorted, preview };
      })
      .sort((a, b) => b.latest - a.latest);
  }, [showTeamGroupByAgent, teamVisibleItems]);

  const jumpToMatch = React.useCallback(
    (direction: "first" | "last") => {
      if (teamFilteredItems.length === 0) return;
      const entry = direction === "first" ? teamFilteredItems[0] : teamFilteredItems[teamFilteredItems.length - 1];
      const ts = typeof entry?.ts === "number" ? entry.ts : 0;
      scrollToTeamMessage(ts);
    },
    [scrollToTeamMessage, teamFilteredItems],
  );

  const jumpToLatest = React.useCallback(() => {
    if (teamFilteredItems.length === 0) return;
    const entry = teamFilteredItems[teamFilteredItems.length - 1];
    const ts = typeof entry?.ts === "number" ? entry.ts : 0;
    scrollToTeamMessage(ts);
  }, [scrollToTeamMessage, teamFilteredItems]);

  const resumeUpdates = React.useCallback(() => {
    setTeamPauseUpdates(false);
    teamPausedItemsRef.current = teamFilteredItems;
    jumpToLatest();
  }, [jumpToLatest, setTeamPauseUpdates, teamFilteredItems]);

  React.useEffect(() => {
    if (teamSearch.trim().length > 0) return;
    const fallback = teamDefaultFilter.trim();
    if (!fallback) return;
    setTeamSearch(fallback);
  }, [setTeamSearch, teamDefaultFilter, teamSearch]);

  React.useEffect(() => {
    if (!showTeamChat || !teamAutoScroll) return;
    if (teamFilteredItems.length === 0) return;
    const last = teamFilteredItems[teamFilteredItems.length - 1];
    const ts = typeof last?.ts === "number" ? last.ts : 0;
    if (!ts || ts === lastAutoScrollTsRef.current) return;
    lastAutoScrollTsRef.current = ts;
    scrollToTeamMessage(ts);
  }, [scrollToTeamMessage, showTeamChat, teamAutoScroll, teamFilteredItems]);

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
          jumpToTeamChat();
        } else if (key === "m") {
          event.preventDefault();
          setShowTeamHeaders((value) => !value);
        } else if (key === "f") {
          event.preventDefault();
          openFilters();
        } else if (key === "l") {
          event.preventDefault();
          jumpToLatest();
        }
      }
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [jumpToLatest, jumpToTeamChat, openFilters, setShowTeamHeaders]);

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
          className={`border ${teamCompactMode ? "rounded-md px-2 py-1" : "rounded-lg px-3 py-2"} ${cardAccent}`}
        >
          {showTeamHeaders ? (
            <div className={`flex flex-wrap items-center gap-2 text-white/60 ${teamCompactMode ? "text-[10px]" : "text-[11px]"}`}>
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
            <div className={`${teamCompactMode ? "text-[10px]" : "text-[11px]"} text-white/60`}>
              <span className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 font-semibold text-white/80">
                {compactLabel}
              </span>
            </div>
          ) : null}
          {highlightSnippet ? (
            <div className={`mt-2 text-amber-100 ${teamCompactMode ? "text-[10px]" : "text-[11px]"}`}>
              <span className="text-amber-200">Match:</span>{" "}
              {highlightSnippet.start > 0 ? "…" : null}
              {renderHighlightedSnippet(highlightSnippet.snippet, highlightNeedle)}
              {highlightSnippet.end < content.length ? "…" : null}
            </div>
          ) : null}
          {content ? (
            isSystem ? (
              <details className={teamCompactMode ? "mt-1" : "mt-2"}>
                <summary className={`cursor-pointer text-white/60 ${teamCompactMode ? "text-[10px]" : "text-[11px]"}`}>
                  System message (collapsed)
                </summary>
                <div className={`mt-2 text-white/90 ${teamCompactMode ? "text-[12px]" : "text-sm"}`}>
                  <Markdown text={String(content)} />
                </div>
              </details>
            ) : (
              <div className={`text-white/90 ${teamCompactMode ? "mt-1 text-[12px] leading-snug" : "mt-2 text-sm"}`}>
                <Markdown text={String(content)} />
              </div>
            )
          ) : null}
          {hasMeta ? (
            <details className={teamCompactMode ? "mt-1" : "mt-2"}>
              <summary className={`cursor-pointer text-white/60 ${teamCompactMode ? "text-[10px]" : "text-[11px]"}`}>
                Message details
              </summary>
              <div className={`mt-2 text-white/60 ${teamCompactMode ? "text-[10px]" : "text-[11px]"}`}>
                {agentLabel ? <div>agent {agentLabel}</div> : null}
                {sessionLabel ? <div>session {sessionLabel}</div> : null}
              </div>
            </details>
          ) : null}
          {uploadNames.length > 0 ? (
            <div className={`text-white/60 ${teamCompactMode ? "mt-1 text-[10px]" : "mt-2 text-[11px]"}`}>
              Shared files: <span className="text-white/80">{uploadNames.join(", ")}</span>
            </div>
          ) : null}
        </div>
      );
    },
    [mutedAgentSet, showTeamHeaders, showTeamRoleLabels, teamCompactMode, teamSearch],
  );

  return {
    filtersActive,
    jumpToLatest,
    jumpToMatch,
    jumpToTeamChat,
    mutedAgentSet,
    openFilters,
    renderTeamMessage,
    resumeUpdates,
    scrollToTeamMessage,
    setShowAllTeamAgents,
    setShowTeamChat,
    setShowTeamGroupByAgent,
    setShowTeamHeaders,
    setShowTeamRoleLabels,
    setShowTeamRunMarkers,
    setShowTeamSystemMessages,
    setTeamAutoScroll,
    setTeamCompactMode,
    setTeamDefaultFilter,
    setTeamFiltersCollapsed,
    setTeamGroupState,
    setTeamHideTools,
    setTeamMutedAgents,
    setTeamPauseUpdates,
    setTeamPinnedFilters,
    setTeamQuietMode,
    setTeamSavedFilters,
    setTeamSearch,
    showAllTeamAgents,
    showTeamChat,
    showTeamGroupByAgent,
    showTeamHeaders,
    showTeamRoleLabels,
    showTeamRunMarkers,
    showTeamSystemMessages,
    teamAgentSummary,
    teamAutoScroll,
    teamCompactMode,
    teamDefaultFilter,
    teamFilteredCount,
    teamFilteredItems,
    teamFiltersCollapsed,
    teamGroupState,
    teamGroupedByAgent,
    teamHideTools,
    teamLastActivity,
    teamMutedAgents,
    teamPauseUpdates,
    teamPendingCount,
    teamPinnedFilters,
    teamQuietMode,
    teamRoleChips,
    teamSavedFilters,
    teamSearch,
    teamSearchRef,
    teamTimelineItems,
    teamVisibleItems,
  };
}

export type HistoryPanelTeamState = ReturnType<typeof useHistoryPanelTeamState>;
