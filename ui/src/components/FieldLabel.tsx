import React from "react";

export default function FieldLabel({ children }: { children: React.ReactNode }) {
  return <div className="text-xs font-medium text-white/70">{children}</div>;
}
