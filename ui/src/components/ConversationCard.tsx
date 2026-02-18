import React from "react";

export default function ConversationCard({
  title,
  children,
}: {
  title: React.ReactNode;
  children: React.ReactNode;
}) {
  return (
    <div className="rounded-lg border border-white/10 bg-white/5">
      <div className="flex items-center justify-between px-3 py-1.5">
        <div className="text-sm font-semibold">{title}</div>
      </div>
      <div className="px-3 pb-2">{children}</div>
    </div>
  );
}
