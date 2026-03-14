import type React from "react";

import type { GraphBuildResult, GraphNode, GraphNodeKind, GraphState } from "../../workflowGraph";

export const WORKFLOW_GRAPH_NODE_WIDTH = 190;
export const WORKFLOW_GRAPH_NODE_HEIGHT = 120;
export const WORKFLOW_GRAPH_NODE_HEADER_HEIGHT = 26;

export type WorkflowGraphComposerProps = {
  state: GraphState;
  onChange: React.Dispatch<React.SetStateAction<GraphState>>;
  buildResult: GraphBuildResult | null;
  buildError?: string | null;
  parseWarnings: string[];
  onImportJson: () => void;
  onExportJson: () => void;
  bearerEnv?: string;
  onClearWarnings: () => void;
};

export type WorkflowGraphSelectionState = {
  selectedId: string | null;
  connectingFrom: string | null;
  selectedNode?: GraphNode;
  nodes: GraphNode[];
  edges: GraphState["edges"];
  nodeMap: Map<string, GraphNode>;
};

export type WorkflowGraphCanvasProps = {
  containerRef: React.RefObject<HTMLDivElement | null>;
  nodes: GraphNode[];
  edges: GraphState["edges"];
  nodeMap: Map<string, GraphNode>;
  selectedId: string | null;
  connectingFrom: string | null;
  onSelectNode: (id: string | null) => void;
  onStartDrag: (id: string, startX: number, startY: number, originX: number, originY: number) => void;
  onConnectStart: (id: string) => void;
  onConnectEnd: (id: string) => void;
  onCanvasPointerDown: () => void;
};

export type WorkflowGraphInspectorProps = {
  selectedNode?: GraphNode;
  edges: GraphState["edges"];
  onRenameNode: (id: string, nextId: string) => void;
  onUpdateNode: (id: string, patch: Partial<GraphNode>) => void;
  onRemoveEdge: (from: string, to: string) => void;
  onRemoveNode: (id: string) => void;
};

export type WorkflowGraphToolbarProps = {
  parseWarnings: string[];
  buildWarnings: string[];
  buildError?: string | null;
  hasRemoteNodes: boolean;
  bearerEnv?: string;
  connectingFrom: string | null;
  onAddNode: (kind: GraphNodeKind) => void;
  onAutoLayout: () => void;
  onImportJson: () => void;
  onExportJson: () => void;
  onResetGraph: () => void;
  onClearWarnings: () => void;
  onCancelConnect: () => void;
};
