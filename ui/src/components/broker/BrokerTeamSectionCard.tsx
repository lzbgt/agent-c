import React from "react";

export type SectionCardProps = {
  title: string;
  description?: string;
  defaultOpen?: boolean;
  children: React.ReactNode;
};

export default function SectionCard({ title, description, defaultOpen = false, children }: SectionCardProps) {
  const [open, setOpen] = React.useState<boolean>(defaultOpen);
  return (
    <details
      className="rounded-md border border-white/10 bg-black/20 p-3"
      open={open}
      onToggle={(event) => setOpen((event.currentTarget as HTMLDetailsElement).open)}
    >
      <summary className="cursor-pointer select-none text-xs font-semibold text-white/80">
        <div className="flex items-center justify-between gap-2">
          <span>{title}</span>
          <span className="text-[11px] text-white/40">Toggle</span>
        </div>
        {description ? <div className="text-[11px] font-normal text-white/50">{description}</div> : null}
      </summary>
      <div className="mt-3 grid gap-3">{children}</div>
    </details>
  );
}
