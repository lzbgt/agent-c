import React from "react";
import ChatThread from "./history/ChatThread";
import useOmniVoice from "../voice/useOmniVoice";
import { ToolError } from "./ToolOutput";

export type VoicePanelProps = { sessionId: string };
function VoiceIcon({ kind }: { kind: "mic" | "camera" | "image" | "speaker" | "end" }) {
  const paths = {
    mic: <><rect x="9" y="2" width="6" height="12" rx="3" /><path d="M5 10v2a7 7 0 0 0 14 0v-2M12 19v3M8 22h8" /></>,
    camera: <><rect x="3" y="6" width="12" height="12" rx="2" /><path d="m15 10 6-3v10l-6-3" /></>,
    image: <><rect x="3" y="3" width="18" height="18" rx="2" /><circle cx="8" cy="8" r="1.5" /><path d="m21 15-6-6L3 21" /></>,
    speaker: <><path d="M11 5 6 9H3v6h3l5 4ZM15 8a6 6 0 0 1 0 8M18 5a10 10 0 0 1 0 14" /></>,
    end: <rect x="6" y="6" width="12" height="12" rx="2" />,
  };
  return <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">{paths[kind]}</svg>;
}
export default function VoicePanel({ sessionId }: VoicePanelProps) {
  const voice = useOmniVoice(sessionId);
  const file = React.useRef<HTMLInputElement>(null);
  const connected = voice.active && voice.phase !== "connecting";
  const available = voice.config.data?.available === true;
  const status = voice.config.isLoading ? "Loading…" : !available ? "Voice unavailable" : ({ ready: "Ready", connecting: "Connecting…", listening: voice.microphoneMuted ? "Microphone muted" : "Listening", thinking: "Thinking…", speaking: "Speaking", ended: "Call ended" })[voice.phase];
  return <section className="voice-workspace" data-testid="voice-panel" aria-label="Voice conversation">
    <div className={`voice-call voice-${voice.phase}`}>
      <span className="voice-provider">Aliyun Omni</span>
      <div className="voice-orb" aria-hidden="true"><VoiceIcon kind="mic" /><span /><span /></div>
      <div className="voice-status" role="status">{status}</div>
      {voice.active ? <time className="voice-time" aria-label="Call time remaining">{Math.floor(voice.remaining / 60)}:{String(voice.remaining % 60).padStart(2, "0")}</time> : null}
      {!voice.active ? <button className="workspace-button voice-start" data-testid="voice-call-start" type="button" disabled={!available || voice.config.isLoading} onClick={() => void voice.start()}><VoiceIcon kind="mic" />Start voice call</button> : <div className="voice-controls">
        <button type="button" className="workspace-button" disabled={!connected} aria-pressed={voice.microphoneMuted} onClick={voice.toggleMicrophone}><VoiceIcon kind="mic" />{voice.microphoneMuted ? "Unmute" : "Mute"}</button>
        <button type="button" className="workspace-button" disabled={!connected} aria-pressed={voice.speakerMuted} onClick={voice.toggleSpeaker}><VoiceIcon kind="speaker" />{voice.speakerMuted ? "Speaker off" : "Speaker on"}</button>
        <button type="button" className="workspace-button" disabled={!connected} aria-pressed={voice.cameraOn} onClick={() => void voice.toggleCamera()}><VoiceIcon kind="camera" />{voice.cameraOn ? "Stop camera" : "Share camera"}</button>
        <button type="button" className="workspace-button" disabled={!connected} onClick={() => file.current?.click()}><VoiceIcon kind="image" />Share image</button>
        <button type="button" className="workspace-button voice-end" data-testid="voice-call-end" onClick={voice.stop}><VoiceIcon kind="end" />End call</button>
      </div>}
      <input ref={file} type="file" accept="image/*" hidden data-testid="voice-image-input" onChange={(event) => { const image = event.target.files?.[0]; event.currentTarget.value = ""; if (image) void voice.shareFile(image); }} />
      <ToolError error={voice.error} />
      <video ref={voice.video} autoPlay muted playsInline hidden={!voice.cameraOn} className="voice-preview" aria-label="Shared camera" />
      {voice.image && !voice.cameraOn ? <div className="voice-image"><img className="voice-preview" src={voice.image} alt="Shared image" /><button type="button" className="workspace-button" onClick={voice.removeImage}>Remove image</button></div> : null}
    </div>
    {voice.messages.length ? <ChatThread messages={voice.messages} working={false} /> : null}
  </section>;
}
