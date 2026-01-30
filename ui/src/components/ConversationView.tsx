import React from "react";
import { apiPostSessionUiEvent, type AgentEvent } from "../api";
import Markdown from "./Markdown";
import ToolResultView from "./ToolResultView";
import ArtifactView from "./ArtifactView";

function safeJsonParse(s: string): any | null {
  try {
    return JSON.parse(s);
  } catch {
    return null;
  }
}

function normalizeEventData(data: unknown): any {
  if (typeof data === "string") {
    return safeJsonParse(data) ?? data;
  }
  return data ?? {};
}

function Card({
  title,
  children,
}: {
  title: React.ReactNode;
  children: React.ReactNode;
}) {
  return (
    <div className="rounded-lg border border-white/10 bg-white/5">
      <div className="flex items-center justify-between px-3 py-2">
        <div className="text-sm font-semibold">{title}</div>
      </div>
      <div className="px-3 pb-3">{children}</div>
    </div>
  );
}

function prettyJsonOrRaw(s: string) {
  const parsed = safeJsonParse(s);
  if (!parsed) return s;
  return JSON.stringify(parsed, null, 2);
}

function safeTrunc(s: string, max: number): string {
  if (s.length <= max) return s;
  return s.slice(0, Math.max(0, max - 1)) + "…";
}

function isSensitiveKey(k: string): boolean {
  const s = String(k || "").toLowerCase();
  return (
    s.includes("secret") ||
    s.includes("token") ||
    s.includes("auth") ||
    s.includes("apikey") ||
    s.includes("api_key") ||
    s.includes("password") ||
    s.includes("passwd") ||
    s.includes("session") ||
    s.includes("cookie")
  );
}

function tryParseUrl(s: string): URL | null {
  try {
    return new URL(s);
  } catch {
    return null;
  }
}

export default function ConversationView({
  baseUrl,
  yolo,
  sessionId,
  client,
  daemonAuthToken,
  prompt,
  events,
  showDebugEvents,
  allowAutoplay,
  allowClientProbes,
}: {
  baseUrl: string;
  yolo: boolean;
  sessionId?: string;
  client?: { id?: string; kind?: string; instance_id?: string };
  daemonAuthToken?: string;
  prompt: string;
  events: AgentEvent[];
  showDebugEvents?: boolean;
  allowAutoplay: boolean;
  allowClientProbes: boolean;
}) {
  const [ackError, setAckError] = React.useState<string | null>(null);
  const [ackedKeys, setAckedKeys] = React.useState<Record<string, boolean>>({});
  const shownUiActionRef = React.useRef<Record<string, boolean>>({});

  const postClientEvent = React.useCallback(
    async (type: string, data: any) => {
      const sid = typeof sessionId === "string" ? sessionId.trim() : "";
      if (sid.length === 0) {
        throw new Error("missing session_id");
      }
      const resp = await apiPostSessionUiEvent(
        baseUrl,
        {
          session_id: sid,
          type,
          client: client ?? { id: "webui", kind: "webui" },
          data: data ?? {},
          append_to_session: false,
        },
        daemonAuthToken,
      );
      if (!resp.ok) {
        throw new Error(resp.error || "client_event failed");
      }
    },
    [baseUrl, client, daemonAuthToken, sessionId],
  );

  // Fundamental DoD handshake: when the UI renders a derived ui_action event, emit a client event so the agent
  // can deterministically stop repeating the same “show/play/notify” requests.
  React.useEffect(() => {
    const sid = typeof sessionId === "string" ? sessionId.trim() : "";
    if (sid.length === 0) return;
    events.forEach((ev) => {
      if (ev.type !== "ui_action") return;
      const data: any = normalizeEventData(ev.data);
      const toolCallId = String(data?.tool_call_id ?? "");
      if (!toolCallId) return;
      if (shownUiActionRef.current[toolCallId]) return;
      const action = data?.action ?? {};
      const atype = String(action?.type ?? "");
      shownUiActionRef.current[toolCallId] = true;
      void postClientEvent("ui_action_shown", { tool_call_id: toolCallId, action_type: atype, title: action?.title }).catch(() => {});
    });
  }, [events, postClientEvent, sessionId]);

  const items: Array<React.ReactNode> = [];
  let streamedAssistant = "";
  let sawFinalAssistant = false;
  let sawToolOrAssistant = false;
  let lastHeartbeat: any = null;

  const probeRanRef = React.useRef<Record<string, boolean>>({});

  if (prompt.trim().length > 0) {
    items.push(
      <Card key="user" title="User">
        <Markdown text={prompt} />
      </Card>,
    );
  }

  events.forEach((ev, idx) => {
    const type = ev.type;
    const data: any = normalizeEventData(ev.data);

    if (type === "assistant_message") {
      sawFinalAssistant = true;
      sawToolOrAssistant = true;
      items.push(
        <Card key={`a-${idx}`} title="Assistant">
          <Markdown text={String(data.assistant_content ?? "")} />
        </Card>,
      );
      return;
    }

    if (type === "assistant_delta") {
      const delta = typeof data.delta === "string" ? data.delta : "";
      if (delta) streamedAssistant += delta;
      if (delta) sawToolOrAssistant = true;
      return;
    }

    if (type === "tool_call") {
      sawToolOrAssistant = true;
      const name = String(data.tool_name ?? "");
      const args = typeof data.arguments_json === "string" ? data.arguments_json : "";
      items.push(
        <Card key={`tc-${idx}`} title={`Tool call: ${name || "(unknown)"}`}>
          <pre className="overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/30 p-3 text-xs leading-relaxed text-white/90">
            {args ? prettyJsonOrRaw(args) : "(enable verbose to capture arguments)"}
          </pre>
        </Card>,
      );
      return;
    }

    if (type === "tool_result") {
      sawToolOrAssistant = true;
      const name = String(data.tool_name ?? "");
      if (typeof data.content === "string") {
        items.push(
          <Card key={`tr-${idx}`} title={`Tool result: ${name || "(unknown)"}`}>
            <ToolResultView baseUrl={baseUrl} yolo={yolo} content={data.content} />
          </Card>,
        );
        return;
      }
      items.push(
        <Card key={`tr-${idx}`} title={`Tool result: ${name || "(unknown)"}`}>
          <pre className="overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/30 p-3 text-xs leading-relaxed text-white/90">
            {data.summary ? JSON.stringify(data.summary, null, 2) : "(enable verbose to capture tool output)"}
          </pre>
        </Card>,
      );
      return;
    }

    if (type === "artifact") {
      sawToolOrAssistant = true;
      const artifact = { ...(data?.artifact ?? {}), tool_call_id: String(data?.tool_call_id ?? "") };
      const title = String(artifact?.title ?? artifact?.path ?? "artifact");
      items.push(
        <Card key={`af-${idx}`} title={`Artifact: ${title}`}>
          <ArtifactView
            baseUrl={baseUrl}
            yolo={yolo}
            artifact={artifact}
            allowAutoplay={allowAutoplay}
            sessionId={sessionId}
            client={client}
            daemonAuthToken={daemonAuthToken}
          />
        </Card>,
      );
      return;
    }

    if (type === "ui_action") {
      sawToolOrAssistant = true;
      const action = data?.action ?? {};
      const atype = String(action?.type ?? "");
      const title = String(action?.title ?? (atype ? `ui_action: ${atype}` : "ui_action"));
      const toolCallId = String(data?.tool_call_id ?? "");

      if (atype === "client_probe") {
        const probeId = String(action?.probe_id ?? toolCallId ?? "").trim();
        const probe = action?.probe ?? {};
        const probeKind = String(probe?.kind ?? "").trim();
        const canRun = !!probeId && typeof sessionId === "string" && sessionId.trim().length > 0;
        const autoRun = !!allowClientProbes && (action?.auto_run === true || action?.auto === true);

        const runProbe = async () => {
          if (!canRun) throw new Error("missing session/probe_id");
          if (!probeKind) throw new Error("missing probe.kind");
          const t0 = Date.now();
          try {
            const safeFieldSet = new Set([
              "tag",
              "text",
              "value",
              "checked",
              "dataset",
              "attrs",
              "currentSrc",
              "paused",
              "ended",
              "currentTime",
              "duration",
            ]);
            const makeDomQuery = () => {
              const selector = safeTrunc(String(probe?.selector ?? ""), 200);
              const limit = Math.min(Math.max(Number(probe?.limit ?? 10) || 10, 1), 20);
              const fieldsRaw = Array.isArray(probe?.fields) ? probe.fields : [];
              let fields = fieldsRaw
                .map((x: any) => String(x))
                .filter((f: string) => safeFieldSet.has(f))
                .slice(0, 20);
              if (fields.length === 0) fields = ["tag", "text"];
              if (!selector) throw new Error("dom_query requires selector");
              const els = Array.from(document.querySelectorAll(selector)).slice(0, limit);
              const items = els.map((el) => {
                const out: any = {};
                const asAny: any = el as any;
                if (fields.includes("tag")) out.tag = el.tagName.toLowerCase();
                if (fields.includes("text")) out.text = safeTrunc(String(el.textContent ?? "").trim(), 400);
                if (fields.includes("value") && "value" in asAny) {
                  const inputType =
                    typeof (asAny as HTMLInputElement)?.type === "string" ? String((asAny as HTMLInputElement).type).toLowerCase() : "";
                  if (inputType === "password") out.value = "(redacted)";
                  else out.value = safeTrunc(String(asAny.value ?? ""), 200);
                }
                if (fields.includes("checked") && "checked" in asAny) out.checked = !!asAny.checked;
                if (fields.includes("dataset")) {
                  const ds: any = asAny.dataset ?? {};
                  const keys = Object.keys(ds).slice(0, 20);
                  const dso: any = {};
                  keys.forEach((k: string) => {
                    dso[k] = isSensitiveKey(k) ? "(redacted)" : safeTrunc(String(ds[k] ?? ""), 200);
                  });
                  out.dataset = dso;
                }
                if (fields.includes("attrs")) {
                  const names = Array.from(el.getAttributeNames ? el.getAttributeNames() : []).slice(0, 20);
                  const attrs: any = {};
                  names.forEach((n) => {
                    attrs[n] = isSensitiveKey(n) ? "(redacted)" : safeTrunc(String(el.getAttribute(n) ?? ""), 200);
                  });
                  out.attrs = attrs;
                }
                if (fields.includes("currentSrc") && "currentSrc" in asAny) out.currentSrc = safeTrunc(String(asAny.currentSrc ?? ""), 300);
                if (fields.includes("paused") && "paused" in asAny) out.paused = !!asAny.paused;
                if (fields.includes("ended") && "ended" in asAny) out.ended = !!asAny.ended;
                if (fields.includes("currentTime") && "currentTime" in asAny) out.currentTime = asAny.currentTime;
                if (fields.includes("duration") && "duration" in asAny) out.duration = asAny.duration;
                return out;
              });
              return { kind: "dom_query", selector, limit, fields, items };
            };

            const makeMediaSnapshot = () => {
              const els = Array.from(document.querySelectorAll("audio,video")).slice(0, 20) as HTMLMediaElement[];
              const items = els.map((el) => {
                const isVideo = el.tagName.toLowerCase() === "video";
                const ds: any = (el as any).dataset || {};
                const src = el.currentSrc || el.src || "";
                const u = src ? tryParseUrl(src) : null;
                const fromFilePath = u && u.pathname.endsWith("/api/v1/file") ? u.searchParams.get("path") || "" : "";
                return {
                  kind: isVideo ? "video" : "audio",
                  tool_call_id: ds.toolCallId || undefined,
                  path: safeTrunc(String(ds.path || fromFilePath || ""), 200),
                  src: safeTrunc(String(src || ""), 300),
                  paused: el.paused,
                  ended: (el as any).ended,
                  current_time: Number.isFinite(el.currentTime) ? el.currentTime : 0,
                  duration: Number.isFinite(el.duration) ? el.duration : 0,
                };
              });
              return { kind: "media_snapshot", items };
            };

            const makeLocation = () => {
              const href = String(window?.location?.href ?? "");
              const u = href ? tryParseUrl(href) : null;
              const sp = u ? u.searchParams : null;
              const searchParams: any = {};
              let hasSensitive = false;
              if (sp) {
                const keys = Array.from(sp.keys()).slice(0, 40);
                keys.forEach((k) => {
                  const v = sp.get(k);
                  const sensitive = isSensitiveKey(k);
                  if (sensitive) hasSensitive = true;
                  searchParams[k] = sensitive ? "(redacted)" : safeTrunc(String(v ?? ""), 200);
                });
              }
              return {
                kind: "location",
                href: safeTrunc(href, 400),
                origin: safeTrunc(String(window?.location?.origin ?? ""), 200),
                pathname: safeTrunc(String(window?.location?.pathname ?? ""), 200),
                search: safeTrunc(String(window?.location?.search ?? ""), 200),
                hash: safeTrunc(String(window?.location?.hash ?? ""), 200),
                search_params: searchParams,
                has_sensitive_query: hasSensitive,
                title: safeTrunc(String(document?.title ?? ""), 200),
              };
            };

            let result: any = null;
            if (probeKind === "dom_query") result = makeDomQuery();
            else if (probeKind === "media_snapshot") result = makeMediaSnapshot();
            else if (probeKind === "location") result = makeLocation();
            else throw new Error(`unsupported probe.kind: ${probeKind}`);

            await postClientEvent("client_probe_result", {
              probe_id: probeId,
              request_tool_call_id: toolCallId,
              probe_kind: probeKind,
              ok: true,
              elapsed_ms: Date.now() - t0,
              result,
            });
          } catch (e) {
            await postClientEvent("client_probe_result", {
              probe_id: probeId,
              request_tool_call_id: toolCallId,
              probe_kind: probeKind,
              ok: false,
              elapsed_ms: Date.now() - t0,
              error: String(e),
            });
          }
        };

        const ackKey = `probe:${probeId || toolCallId || idx}`;
        const alreadyRan = !!probeRanRef.current[ackKey];
        if (autoRun && !alreadyRan && canRun) {
          probeRanRef.current[ackKey] = true;
          void runProbe().catch(() => {});
        }

        items.push(
          <Card key={`ua-${idx}`} title={`UI action: ${title}`}>
            <div className="rounded-md border border-white/10 bg-black/20 px-3 py-2 text-sm text-white/80">
              Client probe requested: <code>{probeKind || "(missing kind)"}</code>
              {probeId ? (
                <span className="ml-2 text-[11px] text-white/50">
                  probe_id=<code>{probeId}</code>
                </span>
              ) : null}
            </div>
            <div className="mt-2 flex flex-wrap items-center gap-2">
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                type="button"
                disabled={!canRun || !allowClientProbes}
                title={!allowClientProbes ? "Enable “Allow agent-requested client probes” in settings to run probes" : ""}
                onClick={() => {
                  probeRanRef.current[ackKey] = true;
                  void runProbe().catch(() => {});
                }}
              >
                Run probe
              </button>
              {!allowClientProbes ? (
                <div className="text-[11px] text-white/40">Disabled by settings</div>
              ) : null}
            </div>
          </Card>,
        );
        return;
      }

      if (atype === "request_client_state" || atype === "request_state") {
        const queryId = String(action?.query_id ?? toolCallId ?? "").trim();
        const canAck = typeof sessionId === "string" && sessionId.trim().length > 0 && !!queryId;
        const ackKey = toolCallId ? `client_state:${toolCallId}` : `client_state:${queryId}`;

        const gatherMediaSnapshot = (): any[] => {
          if (typeof document === "undefined") return [];
          const els = Array.from(document.querySelectorAll("audio,video")).slice(0, 20);
          return els.map((el) => {
            const isVideo = el.tagName.toLowerCase() === "video";
            const m: any = {
              kind: isVideo ? "video" : "audio",
              paused: (el as any).paused,
              ended: (el as any).ended,
            };
            const src = (el as HTMLMediaElement).currentSrc || (el as HTMLMediaElement).src || "";
            if (src) {
              m.src = safeTrunc(src, 300);
              const u = tryParseUrl(src);
              if (u && u.pathname.endsWith("/api/v1/file")) {
                const p = u.searchParams.get("path") || "";
                if (p) m.path = safeTrunc(p, 200);
              }
            }
            const ds: any = (el as any).dataset || {};
            if (typeof ds.toolCallId === "string" && ds.toolCallId.length > 0) m.tool_call_id = ds.toolCallId;
            if (typeof ds.path === "string" && ds.path.length > 0) m.path = safeTrunc(ds.path, 200);

            const ct = (el as any).currentTime;
            if (typeof ct === "number" && Number.isFinite(ct)) m.current_time = ct;
            const dur = (el as any).duration;
            if (typeof dur === "number" && Number.isFinite(dur)) m.duration = dur;
            return m;
          });
        };

        items.push(
          <Card key={`ua-${idx}`} title={`UI action: ${title}`}>
            <div className="rounded-md border border-white/10 bg-black/20 px-3 py-2 text-sm text-white/80">
              Agent requested a client state snapshot.
            </div>
            <div className="mt-2 flex flex-wrap items-center gap-2">
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                type="button"
                disabled={!canAck || !!ackedKeys[ackKey]}
                title={!canAck ? "Select a session first" : toolCallId ? `tool_call_id=${toolCallId}` : ""}
                onClick={() => {
                  setAckError(null);
                  void (async () => {
                    try {
                      await postClientEvent("client_state", {
                        query_id: queryId,
                        request_tool_call_id: toolCallId,
                        url: safeTrunc(String(window?.location?.href ?? ""), 400),
                        media: gatherMediaSnapshot(),
                      });
                      setAckedKeys((prev) => ({ ...prev, [ackKey]: true }));
                    } catch (e) {
                      setAckError(String(e));
                    }
                  })();
                }}
              >
                {ackedKeys[ackKey] ? "Snapshot sent" : "Send snapshot"}
              </button>
              {ackError ? <div className="text-[11px] text-amber-200/80">snapshot failed: {ackError}</div> : null}
            </div>
          </Card>,
        );
        return;
      }

      if (atype === "notify") {
        const msg = String(action?.message ?? "");
        const ackKey = toolCallId ? `tool_call:${toolCallId}` : `notify:${title}:${msg}`;
        const canAck = typeof sessionId === "string" && sessionId.trim().length > 0;
        items.push(
          <Card key={`ua-${idx}`} title={`UI action: ${title}`}>
            <div className="rounded-md border border-white/10 bg-black/20 px-3 py-2 text-sm text-white/80">
              {msg || "(no message)"}
            </div>
            <div className="mt-2 flex flex-wrap items-center gap-2">
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                type="button"
                disabled={!canAck || !!ackedKeys[ackKey]}
                title={!canAck ? "Select a session first" : toolCallId ? `tool_call_id=${toolCallId}` : ""}
                onClick={() => {
                  setAckError(null);
                  void (async () => {
                    try {
                      await postClientEvent("notification_ack", {
                        tool_call_id: toolCallId,
                        action_type: "notify",
                        title,
                        message: msg,
                      });
                      setAckedKeys((prev) => ({ ...prev, [ackKey]: true }));
                    } catch (e) {
                      setAckError(String(e));
                    }
                  })();
                }}
              >
                {ackedKeys[ackKey] ? "Acknowledged" : "Acknowledge"}
              </button>
              {ackError ? <div className="text-[11px] text-amber-200/80">ack failed: {ackError}</div> : null}
            </div>
          </Card>,
        );
        return;
      }
      if (atype === "play_audio") {
        const path = String(action?.path ?? "");
        const artifact = {
          path,
          kind: "audio",
          mime: action?.mime,
          title,
          autoplay: action?.autoplay,
          repeat: action?.repeat,
          source_tool_call_id: toolCallId,
        };
        items.push(
          <Card key={`ua-${idx}`} title={`UI action: ${title}`}>
            <ArtifactView
              baseUrl={baseUrl}
              yolo={yolo}
              artifact={artifact}
              allowAutoplay={allowAutoplay}
              sessionId={sessionId}
              client={client}
              daemonAuthToken={daemonAuthToken}
            />
          </Card>,
        );
        return;
      }

      items.push(
        <Card key={`ua-${idx}`} title={`UI action: ${title}`}>
          <pre className="overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/30 p-3 text-xs leading-relaxed text-white/90">
            {JSON.stringify(action, null, 2)}
          </pre>
        </Card>,
      );
      return;
    }

    if (type === "heartbeat") {
      lastHeartbeat = data ?? {};
      return;
    }

    if (showDebugEvents) {
      if (type === "retry") {
        items.push(
          <Card key={`rt-${idx}`} title="Retry">
            <pre className="overflow-auto whitespace-pre-wrap rounded-md border border-amber-500/30 bg-amber-500/10 p-3 text-xs leading-relaxed text-amber-100">
              {JSON.stringify(data, null, 2)}
            </pre>
          </Card>,
        );
        return;
      }
      if (type === "cancel_requested") {
        items.push(
          <Card key={`cr-${idx}`} title="Cancel requested">
            <pre className="overflow-auto whitespace-pre-wrap rounded-md border border-amber-500/30 bg-amber-500/10 p-3 text-xs leading-relaxed text-amber-100">
              {JSON.stringify(data, null, 2)}
            </pre>
          </Card>,
        );
        return;
      }
      if (type === "cancelled") {
        items.push(
          <Card key={`cx-${idx}`} title="Cancelled">
            <pre className="overflow-auto whitespace-pre-wrap rounded-md border border-rose-500/30 bg-rose-500/10 p-3 text-xs leading-relaxed text-rose-100">
              {JSON.stringify(data, null, 2)}
            </pre>
          </Card>,
        );
        return;
      }
      if (type === "compaction") {
        items.push(
          <Card key={`cp-${idx}`} title="Compaction">
            <pre className="overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/30 p-3 text-xs leading-relaxed text-white/90">
              {JSON.stringify(data, null, 2)}
            </pre>
          </Card>,
        );
        return;
      }
      if (type === "llm_request" || type === "llm_response" || type === "start" || type === "end" || type === "done") {
        items.push(
          <Card key={`dbg-${type}-${idx}`} title={`Debug: ${type}`}>
            <pre className="overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/30 p-3 text-xs leading-relaxed text-white/90">
              {typeof data === "string" ? data : JSON.stringify(data, null, 2)}
            </pre>
          </Card>,
        );
        return;
      }
    }

    // Hide low-level transport/debug events by default; those remain visible in the “Events” timeline.
    if (type === "llm_request" || type === "llm_response") return;
    if (type === "start" || type === "end" || type === "done" || type === "retry" || type === "compaction") return;
    if (type === "error") {
      items.push(
        <Card key={`e-${idx}`} title="Error">
          <pre className="overflow-auto whitespace-pre-wrap rounded-md border border-rose-500/30 bg-rose-500/10 p-3 text-xs leading-relaxed text-rose-100">
            {typeof data === "string" ? data : JSON.stringify(data, null, 2)}
          </pre>
        </Card>,
      );
    }
  });

  // If the daemon is emitting only low-level events (e.g. llm_request/llm_response/start) the conversation view
  // can look "stuck" even though the job is progressing. Provide a lightweight status card in that case.
  if (!sawToolOrAssistant && events.length > 0) {
    const hb = lastHeartbeat && typeof lastHeartbeat === "object" ? JSON.stringify(lastHeartbeat, null, 2) : "";
    items.push(
      <Card key="working" title="Working…">
        <div className="text-xs text-white/70">
          Waiting for assistant output / tool calls.
          {hb ? (
            <pre className="mt-2 overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/30 p-3 text-[11px] leading-relaxed text-white/80">
              {hb}
            </pre>
          ) : null}
        </div>
      </Card>,
    );
  }

  if (!sawFinalAssistant && streamedAssistant.trim().length > 0) {
    items.push(
      <Card key="assistant-stream" title="Assistant (streaming)">
        <Markdown text={streamedAssistant} />
      </Card>,
    );
  }

  return <div className="grid gap-3">{items}</div>;
}
