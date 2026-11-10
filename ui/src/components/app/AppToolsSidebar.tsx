import React from "react";
import WorkspaceIcon from "./WorkspaceIcon";

export type AppAdvancedPage = { id: string; label: string };
type AppToolsSidebarProps = {
  advancedPages: AppAdvancedPage[];
  advancedPage: string;
  toolsCollapsed: boolean;
  setToolsCollapsed: React.Dispatch<React.SetStateAction<boolean>>;
  setAdvancedPage: (next: string) => void;
};

const groups = [
  { label: "Workspace", ids: ["", "workflows"] },
  { label: "Intelligence", ids: ["memory", "trace", "run-diff"] },
  { label: "Operations", ids: ["approvals", "voice", "broker"] },
];

export default function AppToolsSidebar(props: AppToolsSidebarProps) {
  const pages = [{ id: "", label: "Overview" }, ...props.advancedPages];
  return (
    <aside className={`workspace-sidebar ${props.toolsCollapsed ? "is-collapsed" : ""}`}>
      <div className="workspace-sidebar-heading">
        <span>CONTROL CENTER</span>
        <button type="button" className="workspace-collapse" onClick={() => props.setToolsCollapsed((prev) => !prev)} aria-label={props.toolsCollapsed ? "Expand tools" : "Collapse tools"} aria-expanded={!props.toolsCollapsed} title={props.toolsCollapsed ? "Expand tools" : "Collapse tools"}>
          {props.toolsCollapsed ? "»" : "«"}
        </button>
      </div>
      <nav aria-label="Workspace navigation">
        {groups.map((group) => (
          <div className="workspace-nav-group" key={group.label}>
            <div className="workspace-nav-label">{group.label}</div>
            {group.ids.map((id) => {
              const page = pages.find((entry) => entry.id === id);
              if (!page) return null;
              const active = id === props.advancedPage;
              return (
                <button key={id} className={`workspace-nav-item ${active ? "is-active" : ""}`} type="button" onClick={() => props.setAdvancedPage(id)} title={page.label} aria-label={page.label} aria-current={active ? "page" : undefined}>
                  <WorkspaceIcon name={id || "workspace"} />
                  <span>{page.label}</span>
                  {active ? <span className="workspace-nav-indicator" /> : null}
                </button>
              );
            })}
          </div>
        ))}
      </nav>
      <div className="workspace-sidebar-note"><span className="workspace-note-mark">C</span><span>Portable core.<br /><strong>Durable execution.</strong></span></div>
    </aside>
  );
}
