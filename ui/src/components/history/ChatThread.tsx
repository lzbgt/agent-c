import React from "react";
import type { ChatMessage } from "../../history/chatTranscript";
import { parseAssistantMultimodal, renderAssistantMultimodal } from "../AssistantMultimodal";
import Markdown from "../Markdown";

export default function ChatThread({ messages, working, error }: { messages: ChatMessage[]; working: boolean; error?: string }) {
  const root = React.useRef<HTMLDivElement>(null);
  const following = React.useRef(true);
  React.useEffect(() => {
    const scroll = root.current?.closest("#workspace-main");
    if (!scroll) return;
    const onScroll = () => { following.current = scroll.scrollHeight - scroll.scrollTop - scroll.clientHeight < 100; };
    scroll.addEventListener("scroll", onScroll, { passive: true });
    return () => scroll.removeEventListener("scroll", onScroll);
  }, []);
  const lastMessage = messages[messages.length - 1];
  React.useLayoutEffect(() => {
    const scroll = root.current?.closest("#workspace-main");
    if (scroll && following.current) scroll.scrollTop = scroll.scrollHeight;
  }, [messages.length, lastMessage?.content, working]);
  return (
    <div ref={root} className="chat-thread" role="log" aria-label="Conversation" aria-live="polite" aria-relevant="additions text" data-testid="chat-thread">
      {messages.map((message) => <article key={message.key} className={`chat-message chat-${message.role}`} data-testid={`chat-${message.role}`}>
        <div className="chat-author">{message.role === "user" ? "You" : "Agent"}</div>
        <div className="chat-message-body">
          {message.content ? <Markdown text={message.content} /> : null}
          {renderAssistantMultimodal(parseAssistantMultimodal(message.multimodal))}
          {message.truncated ? <span className="text-xs text-amber-200">Message shortened in this view.</span> : null}
        </div>
      </article>)}
      {working ? <div className="chat-working" role="status">Working…</div> : null}
      {error && !working ? <div className="chat-send-error" role="alert">{error}</div> : null}
    </div>
  );
}
