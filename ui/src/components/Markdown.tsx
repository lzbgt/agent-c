import React from "react";
import ReactMarkdown from "react-markdown";
import remarkGfm from "remark-gfm";
import rehypeHighlight from "rehype-highlight";

export default function Markdown({ text }: { text: string }) {
  return (
    <div className="prose prose-sm prose-invert max-w-none break-words prose-p:my-1 prose-ul:my-1 prose-ol:my-1 prose-li:my-0.5 prose-pre:my-2 prose-pre:bg-black/40 prose-pre:border prose-pre:border-white/10 prose-pre:p-2 prose-pre:whitespace-pre-wrap prose-pre:break-words prose-code:text-white prose-code:break-all">
      <ReactMarkdown remarkPlugins={[remarkGfm]} rehypePlugins={[rehypeHighlight]}>
        {text}
      </ReactMarkdown>
    </div>
  );
}
