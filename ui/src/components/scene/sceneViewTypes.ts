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
  props?: Record<string, unknown>;
  created_ms?: number;
  updated_ms?: number;
};

export type SceneClientRef = {
  id?: string;
  kind?: string;
  instance_id?: string;
};

export type SceneEntityMutationOp =
  | {
      op: "create";
      id?: string;
      entity_kind: string;
      title?: string;
      props?: Record<string, unknown>;
    }
  | {
      op: "update";
      id: string;
      props?: Record<string, unknown>;
    }
  | {
      op: "delete" | "remove";
      id: string;
    }
  | {
      op: "action";
      id: string;
      action: string;
      args?: Record<string, unknown>;
    }
  | {
      op: "clear";
      color?: string;
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
  client?: SceneClientRef;
  daemonAuth?: ApiAuth;
  sessionId?: string;
  entities: SceneEntity[];
  className?: string;
};
