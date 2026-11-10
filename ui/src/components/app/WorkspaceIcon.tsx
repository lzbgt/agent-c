import React from "react";

const paths: Record<string, React.ReactNode> = {
  workspace: <><rect x="3" y="3" width="18" height="18" rx="4" /><path d="M3 9h18M9 9v12" /></>,
  workflows: <><rect x="3" y="3" width="6" height="6" rx="2" /><rect x="15" y="15" width="6" height="6" rx="2" /><path d="M9 6h6a3 3 0 0 1 3 3v6M6 9v9h9" /></>,
  memory: <><path d="M9 4a3 3 0 0 0-5 3 4 4 0 0 0 0 7 3 3 0 0 0 5 4M15 4a3 3 0 0 1 5 3 4 4 0 0 1 0 7 3 3 0 0 1-5 4M9 3v18M15 3v18M4 10h5M15 13h5" /></>,
  trace: <path d="M3 12h4l3-8 4 16 3-8h4" />,
  voice: <><rect x="9" y="3" width="6" height="12" rx="3" /><path d="M5 11a7 7 0 0 0 14 0M12 18v3M8 21h8" /></>,
  approvals: <><path d="m12 3 8 3v6c0 5-8 9-8 9s-8-4-8-9V6z" /><path d="m8 12 3 3 5-6" /></>,
  "run-diff": <><circle cx="6" cy="5" r="2" /><circle cx="6" cy="19" r="2" /><circle cx="18" cy="19" r="2" /><path d="M6 7v10M6 9c0 5 12 1 12 8" /></>,
  broker: <><rect x="3" y="3" width="18" height="7" rx="2" /><rect x="3" y="14" width="18" height="7" rx="2" /><path d="M7 6.5h.01M7 17.5h.01M11 6.5h6M11 17.5h6" /></>,
  arrow: <path d="M5 12h14m-5-5 5 5-5 5" />,
};

export default function WorkspaceIcon({ name, className = "" }: { name: string; className?: string }) {
  return <svg className={className} width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">{paths[name] ?? paths.workspace}</svg>;
}
