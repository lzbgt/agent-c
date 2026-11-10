import React from "react";
import { useQuery } from "@tanstack/react-query";
import type { ChatMessage } from "../history/chatTranscript";
import { VoiceAudio, encodeBase64, imageJPEG } from "./VoiceAudio";

type Phase = "ready" | "connecting" | "listening" | "thinking" | "speaking" | "ended";
type Config = { available: boolean; max_session_seconds: number; model: string };
type Runtime = {
  active: boolean; ready: boolean; audioSent: boolean; socket?: WebSocket; audio?: VoiceAudio; timer?: number; startup?: number;
  camera?: MediaStream; cameraTimer?: number; cameraVersion: number; imageTimer?: number; pendingImage?: string; staticImage?: string; lastImage: number;
  responseId: string; responseActive: boolean; speechId: string; inputSpeaking: boolean; audioPlaying: boolean;
  ignored: Set<string>; replies: Map<string, { text: string; speech: string }>;
};
const closedRuntime = (): Runtime => ({ active: false, ready: false, audioSent: false, cameraVersion: 0, lastImage: 0, responseId: "", responseActive: false, speechId: "", inputSpeaking: false, audioPlaying: false, ignored: new Set(), replies: new Map() });

export default function useOmniVoice(sessionId: string) {
  const [phase, setPhase] = React.useState<Phase>("ready");
  const [error, setError] = React.useState<string | null>(null);
  const [messages, setMessages] = React.useState<ChatMessage[]>([]);
  const [microphoneMuted, setMicrophoneMuted] = React.useState(false);
  const [speakerMuted, setSpeakerMuted] = React.useState(false);
  const [cameraOn, setCameraOn] = React.useState(false);
  const [image, setImage] = React.useState<string | null>(null);
  const [remaining, setRemaining] = React.useState(300);
  const runtime = React.useRef<Runtime>(closedRuntime());
  const video = React.useRef<HTMLVideoElement>(null);
  const config = useQuery<Config>({ queryKey: ["omniVoiceConfig"], retry: false, queryFn: async () => {
    const response = await fetch("/voice/v1/config", { credentials: "same-origin" });
    if (!response.ok) throw new Error("Voice is unavailable.");
    return response.json();
  } });
  const current = (r: Runtime) => runtime.current === r && r.active;
  const stopCamera = (r: Runtime) => {
    r.cameraVersion++; window.clearInterval(r.cameraTimer); r.cameraTimer = undefined;
    r.camera?.getTracks().forEach((track) => track.stop()); r.camera = undefined;
    if (video.current) video.current.srcObject = null;
  };
  const dispose = (r: Runtime) => {
    r.active = false; r.ready = false; window.clearInterval(r.timer); window.clearTimeout(r.startup); window.clearTimeout(r.imageTimer);
    stopCamera(r); r.audio?.close();
    if (r.socket) { r.socket.onclose = null; r.socket.onerror = null; r.socket.onmessage = null; r.socket.close(); }
  };
  const stop = (message?: string) => {
    dispose(runtime.current); setCameraOn(false); setPhase("ended"); if (message) setError(message);
  };
  const stopRef = React.useRef(stop); stopRef.current = stop;
  React.useEffect(() => {
    setPhase("ready"); setMessages([]); setImage(null); setError(null); setCameraOn(false);
    return () => { dispose(runtime.current); };
  }, [sessionId]);

  const syncPhase = (r: Runtime) => { if (current(r)) setPhase(!r.ready ? "connecting" : r.inputSpeaking ? "listening" : r.audioPlaying ? "speaking" : r.responseActive ? "thinking" : "listening"); };
  const updateMessage = (key: string, role: "user" | "assistant", content: string) => {
    if (!content) return;
    content = content.slice(0, 65536);
    setMessages((previous) => {
      const index = previous.findIndex((item) => item.key === key);
      if (index < 0) return [...previous, { key, role, content }].slice(-100);
      return previous.map((item, i) => i === index ? { ...item, content } : item);
    });
  };
  const send = (r: Runtime, type: string, data: Record<string, unknown> = {}) => {
    if (!current(r) || !r.ready || r.socket?.readyState !== WebSocket.OPEN) return false;
    if (r.socket.bufferedAmount > 512 * 1024) { stopRef.current("Voice connection is too slow. Start a new call."); return false; }
    r.socket.send(JSON.stringify({ type, event_id: crypto.randomUUID(), ...data })); return true;
  };
  const sendImage = (r: Runtime, data: string) => {
    if (!current(r)) return;
    r.pendingImage = data;
    if (r.imageTimer) return;
    const flush = () => {
      r.imageTimer = undefined;
      if (!current(r) || !r.pendingImage) return;
      if (!r.audioSent) { r.imageTimer = window.setTimeout(flush, 50); return; }
      const frame = r.pendingImage; r.pendingImage = undefined;
      if (send(r, "input_image_buffer.append", { image: frame })) r.lastImage = Date.now();
    };
    const wait = Math.max(0, 1050 - (Date.now() - r.lastImage));
    if (wait) r.imageTimer = window.setTimeout(flush, wait); else flush();
  };
  const receive = (r: Runtime, data: Record<string, any>) => {
    if (!current(r)) return;
    const type = data.type;
    if (type === "session.updated" && !r.ready) {
      r.ready = true; window.clearTimeout(r.startup);
      const seconds = Math.min(300, config.data?.max_session_seconds || 300); const deadline = Date.now() + seconds * 1000;
      setRemaining(seconds); r.timer = window.setInterval(() => { if (!current(r)) return; const value = Math.max(0, Math.ceil((deadline - Date.now()) / 1000)); setRemaining(value); if (r.staticImage && !r.camera) sendImage(r, r.staticImage); if (!value) stopRef.current(); }, 1000);
    }
    if (type === "input_audio_buffer.speech_started") {
      r.speechId = String(data.item_id || crypto.randomUUID()); r.inputSpeaking = true; r.audio?.resetPlayback();
      if (r.responseActive && r.responseId) { r.ignored.add(r.responseId); if (r.ignored.size > 128) r.ignored.delete(r.ignored.values().next().value!); send(r, "response.cancel"); }
    }
    if (type === "input_audio_buffer.speech_stopped") r.inputSpeaking = false;
    if (type === "conversation.item.input_audio_transcription.delta" || type === "conversation.item.input_audio_transcription.completed") {
      const key = String(data.item_id || r.speechId || "speech");
      updateMessage(`input:${key}`, "user", type.endsWith("completed") ? String(data.transcript || "") : String(data.text || "") + String(data.stash || ""));
    }
    if (type === "response.created") { r.responseId = String(data.response?.id || crypto.randomUUID()); r.responseActive = true; }
    const responseId = String(data.response_id || r.responseId);
    if (type === "response.audio.delta" && !r.ignored.has(responseId) && typeof data.delta === "string") r.audio?.play(data.delta);
    if (["response.text.delta", "response.text.done", "response.audio_transcript.delta", "response.audio_transcript.done"].includes(type) && !r.ignored.has(responseId)) {
      const reply = r.replies.get(responseId) || { text: "", speech: "" }; const field = type.startsWith("response.text") ? "text" : "speech";
      const text = String(type.endsWith("delta") ? data.delta || "" : data.text || data.transcript || "");
      reply[field] = (type.endsWith("delta") ? reply[field] + text : text).slice(0, 65536); r.replies.set(responseId, reply);
      if (r.replies.size > 100) r.replies.delete(r.replies.keys().next().value!);
      updateMessage(`output:${responseId}`, "assistant", reply.speech || reply.text);
    }
    if (type === "response.done") { if (!data.response?.id || data.response.id === r.responseId) r.responseActive = false; if (data.response?.status === "failed") setError("The voice reply could not be completed."); }
    if (type === "error") { const code = String(data.error?.code || ""); if (!/response_cancel|response_not_active/.test(code)) { stopRef.current(String(data.error?.message || "Voice is temporarily unavailable.").slice(0, 240)); return; } }
    syncPhase(r);
  };
  const start = async () => {
    if (runtime.current.active || !config.data?.available) return;
    dispose(runtime.current); const r = closedRuntime(); r.active = true; runtime.current = r;
    setError(null); setMessages([]); setImage(null); setCameraOn(false); setMicrophoneMuted(false); setSpeakerMuted(false); setPhase("connecting");
    try {
      r.audio = new VoiceAudio((bytes) => { if (send(r, "input_audio_buffer.append", { audio: encodeBase64(bytes) })) r.audioSent = true; }, (playing) => { if (current(r)) { r.audioPlaying = playing; syncPhase(r); } }, (message) => { if (current(r)) stopRef.current(message); });
      await r.audio.start(); if (!current(r)) return;
      const url = new URL("/voice/v1/realtime", window.location.href); url.protocol = url.protocol === "https:" ? "wss:" : "ws:";
      r.socket = new WebSocket(url);
      r.startup = window.setTimeout(() => { if (current(r) && !r.ready) stopRef.current("Voice could not connect. Try again."); }, 20000);
      r.socket.onmessage = ({ data }) => { try { receive(r, JSON.parse(String(data))); } catch { if (current(r)) stopRef.current("Voice returned an unexpected response."); } };
      r.socket.onerror = () => { if (current(r)) stopRef.current("Voice could not connect. Try again."); };
      r.socket.onclose = () => { if (current(r)) stopRef.current("Voice connection closed. Start a new call."); };
    } catch (cause) {
      if (!current(r)) return;
      stopRef.current(cause instanceof DOMException && cause.name === "NotAllowedError" ? "Microphone access is disabled." : "Voice input could not start.");
    }
  };
  const shareFile = async (file: File) => {
    const r = runtime.current; if (!current(r) || !r.ready) return;
    try {
      if (file.size > 10 * 1024 * 1024) throw new Error("Choose an image smaller than 10 MB.");
      const bitmap = await createImageBitmap(file);
      let encoded: string; try { encoded = await imageJPEG(bitmap, bitmap.width, bitmap.height); } finally { bitmap.close(); }
      if (current(r)) { r.staticImage = encoded; sendImage(r, encoded); setImage(`data:image/jpeg;base64,${encoded}`); }
    } catch (cause) { if (current(r)) setError(cause instanceof Error ? cause.message : "This image could not be shared."); }
  };
  const toggleCamera = async () => {
    const r = runtime.current; if (!current(r) || !r.ready) return;
    if (r.camera) { stopCamera(r); setCameraOn(false); return; }
    const version = ++r.cameraVersion;
    try {
      const stream = await navigator.mediaDevices.getUserMedia({ video: { width: { ideal: 1280 }, height: { ideal: 720 }, facingMode: "environment" }, audio: false });
      if (!current(r) || version !== r.cameraVersion || !video.current) { stream.getTracks().forEach((track) => track.stop()); return; }
      r.camera = stream; video.current.srcObject = stream; await video.current.play();
      if (!current(r) || version !== r.cameraVersion) return;
      setCameraOn(true); let encoding = false;
      const capture = async () => {
        const source = video.current; if (!source || !source.videoWidth || encoding || !current(r)) return;
        encoding = true;
        try { const image = await imageJPEG(source, source.videoWidth, source.videoHeight); if (current(r) && version === r.cameraVersion) sendImage(r, image); }
        catch { if (current(r) && version === r.cameraVersion) { stopCamera(r); setCameraOn(false); setError("Camera sharing stopped."); } }
        finally { encoding = false; }
      };
      void capture(); r.cameraTimer = window.setInterval(() => void capture(), 1100);
    } catch { if (current(r) && version === r.cameraVersion) { stopCamera(r); setCameraOn(false); setError("Camera access is unavailable."); } }
  };
  return { config, phase, error, messages, microphoneMuted, speakerMuted, cameraOn, image, remaining, video,
    active: phase === "connecting" || phase === "listening" || phase === "thinking" || phase === "speaking",
    start, stop: () => stop(), shareFile, toggleCamera,
    removeImage: () => { const r = runtime.current; r.staticImage = undefined; r.pendingImage = undefined; window.clearTimeout(r.imageTimer); r.imageTimer = undefined; setImage(null); },
    toggleMicrophone: () => { const value = !microphoneMuted; runtime.current.audio?.muteMicrophone(value); setMicrophoneMuted(value); },
    toggleSpeaker: () => { const value = !speakerMuted; runtime.current.audio?.muteSpeaker(value); setSpeakerMuted(value); },
  };
}
