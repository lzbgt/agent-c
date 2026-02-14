import type { AgentEvent } from "./api";

const LIVE_EVENTS_MAX = 2000;

export function capLiveEvents(events: AgentEvent[]): AgentEvent[] {
  if (!Array.isArray(events) || events.length <= LIVE_EVENTS_MAX) return events;
  return events.slice(events.length - LIVE_EVENTS_MAX);
}

export function appendLiveEvents(prev: AgentEvent[], next: AgentEvent | AgentEvent[]): AgentEvent[] {
  const chunk = Array.isArray(next) ? next : [next];
  if (chunk.length === 0) return prev;
  return capLiveEvents(prev.concat(chunk));
}
