import React from "react";

export type AppAdvancedPage = {
  id: string;
  label: string;
};

type AppToolsSidebarProps = {
  advancedPages: AppAdvancedPage[];
  advancedPage: string;
  toolsCollapsed: boolean;
  setToolsCollapsed: React.Dispatch<React.SetStateAction<boolean>>;
  setAdvancedPage: (next: string) => void;
};

export default function AppToolsSidebar(props: AppToolsSidebarProps) {
  return (
    <aside
      className={`rounded-lg border border-white/10 bg-black/20 ${props.toolsCollapsed ? "px-2 py-3" : "p-3"} lg:sticky lg:top-4 lg:self-start`}
    >
      <div className="flex items-center justify-between gap-2">
        <div className="text-[11px] font-semibold text-white/60">Tools</div>
        <button
          className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-[10px] text-white/70 hover:bg-black/40"
          type="button"
          onClick={() => props.setToolsCollapsed((prev) => !prev)}
          title={props.toolsCollapsed ? "Expand tools" : "Collapse tools"}
        >
          {props.toolsCollapsed ? "»" : "«"}
        </button>
      </div>
      <div className="mt-2 grid gap-1">
        {props.advancedPages.map((page) => {
          const active = page.id === props.advancedPage;
          const shortLabel = page.label.slice(0, 3).toUpperCase();
          return (
            <button
              key={page.id}
              className={`rounded-md ${props.toolsCollapsed ? "px-2 py-2 text-center text-[11px]" : "px-3 py-2 text-left text-sm"} ${
                active ? "bg-indigo-500/20 text-indigo-100" : "bg-black/20 text-white/70 hover:bg-black/30"
              }`}
              type="button"
              onClick={() => props.setAdvancedPage(page.id)}
              title={page.label}
            >
              {props.toolsCollapsed ? shortLabel : page.label}
            </button>
          );
        })}
      </div>
    </aside>
  );
}
