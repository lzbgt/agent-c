import type { SceneEntity } from "./components/SceneView";

export const SCENE_STORE_MAX = 12;

export type SceneStoreMap = Record<string, Record<string, SceneEntity>>;
export type SceneUpdatedMap = Record<string, number>;

export function touchSceneStoreKey(
  store: SceneStoreMap,
  order: string[],
  updated: SceneUpdatedMap,
  key: string,
  maxEntries: number = SCENE_STORE_MAX,
): void {
  if (!key) return;
  const idx = order.indexOf(key);
  if (idx >= 0) order.splice(idx, 1);
  order.push(key);

  if (maxEntries <= 0 || order.length <= maxEntries) return;
  const evictCount = order.length - maxEntries;
  const evicted = order.splice(0, evictCount);
  for (const k of evicted) {
    delete store[k];
    delete updated[k];
  }
}
