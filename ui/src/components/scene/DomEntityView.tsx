import type { ApiAuth } from "../../api";
import type { SceneEntity, SceneScriptErrorArgs } from "./sceneViewTypes";
import { safeObject, safeString } from "./sceneViewUtils";
import SceneScriptFrame from "./SceneScriptFrame";

type Props = { entity: SceneEntity; baseUrl?: string; yolo?: boolean; allowAutoplay?: boolean; sessionId?: string; daemonAuth?: ApiAuth; onScriptError?: (args: SceneScriptErrorArgs) => void };

export default function DomEntityView(props: Props) {
  const values = safeObject(props.entity.props);
  const html = safeString(values.html ?? values.markup ?? values.inner_html ?? values.innerHTML).slice(0, 1_000_000);
  const script = safeString(values.script ?? values.js ?? values.code).slice(0, 1_000_000);
  return <SceneScriptFrame {...props} kind="dom" script={script} html={html} />;
}
