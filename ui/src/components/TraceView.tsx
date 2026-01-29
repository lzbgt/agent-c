import React from "react";

export default function TraceView({ trace }: { trace: string }) {
  const [open, setOpen] = React.useState(true);
  return (
    <div className="rounded-lg border border-white/10 bg-white/5">
      <div className="flex items-center justify-between px-3 py-2">
        <div className="text-sm font-semibold">Transcript</div>
        <button
          className="text-xs text-white/70 hover:text-white"
          onClick={() => setOpen((v) => !v)}
          type="button"
        >
          {open ? "Collapse" : "Expand"}
        </button>
      </div>
      {open ? (
        <pre className="max-h-[520px] overflow-auto whitespace-pre-wrap px-3 pb-3 text-xs leading-relaxed text-white/90">
          {trace || "(empty)"}
        </pre>
      ) : null}
    </div>
  );
}

