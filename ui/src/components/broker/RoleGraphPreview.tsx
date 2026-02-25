import React from "react";
import type { RoleGraphEdge } from "./teamRunUtils";

type RoleGraphPreviewProps = {
  roles: string[];
  edges: RoleGraphEdge[];
  title?: string;
  emptyLabel?: string;
};

const normalizeRole = (raw: string) => String(raw || "").trim().toLowerCase();

const buildRoleGraphLayout = (roles: string[]) => {
  const size = 220;
  const center = size / 2;
  const radius = Math.max(60, Math.min(90, 10 * roles.length));
  const nodes: Record<string, { x: number; y: number }> = {};
  const count = roles.length;
  for (let i = 0; i < count; i += 1) {
    const angle = (2 * Math.PI * i) / count - Math.PI / 2;
    nodes[roles[i]] = {
      x: center + radius * Math.cos(angle),
      y: center + radius * Math.sin(angle),
    };
  }
  return { size, center, radius, nodes };
};

export default function RoleGraphPreview(props: RoleGraphPreviewProps) {
  const roles = Array.isArray(props.roles) ? props.roles : [];
  const edges = Array.isArray(props.edges) ? props.edges : [];
  const markerId = React.useId();
  if (roles.length === 0) {
    return <div className="text-[11px] text-white/40">{props.emptyLabel || "No role graph recorded."}</div>;
  }
  const layout = buildRoleGraphLayout(roles);
  const title = props.title || "Role graph preview";
  return (
    <div className="rounded-md border border-white/10 bg-black/20 p-2">
      <div className="text-[11px] text-white/60">{title}</div>
      <svg className="mt-2 w-full" viewBox={`0 0 ${layout.size} ${layout.size}`} role="img" aria-label={title}>
        <defs>
          <marker
            id={markerId}
            markerWidth="6"
            markerHeight="6"
            refX="5"
            refY="3"
            orient="auto"
            markerUnits="strokeWidth"
          >
            <path d="M0,0 L6,3 L0,6 z" fill="#93c5fd" />
          </marker>
        </defs>
        {edges.map((edge, idx) => {
          const from = layout.nodes[normalizeRole(edge.from_role)];
          const to = layout.nodes[normalizeRole(edge.to_role)];
          if (!from || !to) return null;
          return (
            <line
              key={`role-edge-line-${edge.from_role}-${edge.to_role}-${idx}`}
              x1={from.x}
              y1={from.y}
              x2={to.x}
              y2={to.y}
              stroke="#93c5fd"
              strokeWidth="1.5"
              markerEnd={`url(#${markerId})`}
              opacity="0.7"
            />
          );
        })}
        {roles.map((role) => {
          const node = layout.nodes[role];
          if (!node) return null;
          return (
            <g key={`role-node-${role}`}>
              <circle cx={node.x} cy={node.y} r="14" fill="#0f172a" stroke="#38bdf8" strokeWidth="1" />
              <text x={node.x} y={node.y + 4} textAnchor="middle" fontSize="10" fill="#e2e8f0">
                {role}
              </text>
            </g>
          );
        })}
      </svg>
      {edges.length > 0 ? (
        <div className="mt-2 text-[11px] text-white/50">
          {edges.length} edge{edges.length === 1 ? "" : "s"}
        </div>
      ) : null}
    </div>
  );
}
