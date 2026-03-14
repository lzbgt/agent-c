import type { ApiAuth } from "../../api";

export type CanvasPoint = { x: number; y: number };

export type DrawOp =
  | { op: "clear"; color?: string }
  | { op: "polyline"; points: CanvasPoint[]; strokeStyle?: string; lineWidth?: number }
  | { op: "line"; x1: number; y1: number; x2: number; y2: number; strokeStyle?: string; lineWidth?: number }
  | { op: "text"; x: number; y: number; text: string; fillStyle?: string; font?: string };

export type SceneEntity = {
  id: string;
  kind: string;
  title?: string;
  props?: any;
  created_ms?: number;
  updated_ms?: number;
};

export type SceneScriptErrorArgs = {
  entity_id: string;
  entity_kind: string;
  error: string;
  stack_preview?: string;
  script_preview?: string;
};

export type SceneViewProps = {
  baseUrl?: string;
  yolo?: boolean;
  allowAutoplay?: boolean;
  client?: any;
  daemonAuth?: ApiAuth;
  sessionId?: string;
  entities: SceneEntity[];
  className?: string;
};
