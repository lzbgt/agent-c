import React from "react";
import type { BrokerAudioSignalEvent } from "../../api";

type UseBrokerAudioWebRtcStateArgs = {
  canQuery: boolean;
  selectedSessionId: string;
  signalEvents: BrokerAudioSignalEvent[];
  sendSignalDirect: (type: string, payload?: Record<string, unknown>) => Promise<unknown>;
};

type WebRtcWindow = Window & {
  __agentuiRtcFactory?: () => RTCPeerConnection;
};

type WebRtcConnectState = "idle" | "connecting" | "connected" | "closed" | "remote_closed";

function makeSignalKey(ev: BrokerAudioSignalEvent): string {
  let payload = "";
  try {
    payload = JSON.stringify(ev.payload ?? null);
  } catch {
    payload = "[unserializable]";
  }
  return `${ev.ts_unix_ms}:${ev.from || ""}:${ev.type}:${payload}`;
}

function asRecord(value: unknown): Record<string, unknown> | null {
  if (!value || typeof value !== "object" || Array.isArray(value)) return null;
  return value as Record<string, unknown>;
}

function normalizeSdpForBrowser(sdp: string): string {
  if (!sdp) return sdp;
  return sdp.endsWith("\n") ? sdp : `${sdp}\r\n`;
}

function parseSessionDescription(type: "offer" | "answer", payload: unknown): RTCSessionDescriptionInit | null {
  const record = asRecord(payload);
  if (!record) return null;
  const sdp = typeof record.sdp === "string" ? record.sdp : "";
  if (!sdp) return null;
  const descType = typeof record.type === "string" && record.type ? record.type : type;
  return { type: descType as RTCSdpType, sdp: normalizeSdpForBrowser(sdp) };
}

function parseIceCandidate(payload: unknown): RTCIceCandidateInit | null {
  const record = asRecord(payload);
  if (!record) return null;
  if (typeof record.candidate !== "string" || !record.candidate) return null;
  const out: RTCIceCandidateInit = { candidate: record.candidate };
  if (typeof record.sdpMid === "string") out.sdpMid = record.sdpMid;
  if (typeof record.sdpMLineIndex === "number") out.sdpMLineIndex = record.sdpMLineIndex;
  if (typeof record.usernameFragment === "string") out.usernameFragment = record.usernameFragment;
  return out;
}

export default function useBrokerAudioWebRtcState(args: UseBrokerAudioWebRtcStateArgs) {
  const supported =
    typeof window !== "undefined" &&
    (typeof (window as WebRtcWindow).__agentuiRtcFactory === "function" || typeof window.RTCPeerConnection === "function");
  const audioRef = React.useRef<HTMLAudioElement | null>(null);
  const peerRef = React.useRef<RTCPeerConnection | null>(null);
  const signalEventsRef = React.useRef<BrokerAudioSignalEvent[]>(args.signalEvents);
  const handledSignalsRef = React.useRef<Set<string>>(new Set());
  const remoteTrackIdsRef = React.useRef<Set<string>>(new Set());

  const [connectState, setConnectState] = React.useState<WebRtcConnectState>("idle");
  const [connectionState, setConnectionState] = React.useState<string>("new");
  const [signalingState, setSignalingState] = React.useState<string>("stable");
  const [iceConnectionState, setIceConnectionState] = React.useState<string>("new");
  const [remoteTrackCount, setRemoteTrackCount] = React.useState<number>(0);
  const [sentCandidateCount, setSentCandidateCount] = React.useState<number>(0);
  const [receivedCandidateCount, setReceivedCandidateCount] = React.useState<number>(0);
  const [lastRemoteSignal, setLastRemoteSignal] = React.useState<string>("");
  const [webrtcError, setWebrtcError] = React.useState<string | null>(null);
  const [connectPending, setConnectPending] = React.useState<boolean>(false);

  React.useEffect(() => {
    signalEventsRef.current = args.signalEvents;
  }, [args.signalEvents]);

  const clearAudio = React.useCallback(() => {
    const el = audioRef.current;
    if (!el) return;
    try {
      el.pause();
    } catch {
      // ignore
    }
    try {
      el.srcObject = null;
    } catch {
      // ignore
    }
    el.removeAttribute("src");
  }, []);

  const closePeerLocal = React.useCallback(
    (peer: RTCPeerConnection | null) => {
      if (peer) {
        try {
          peer.onicecandidate = null;
          peer.ontrack = null;
          peer.onconnectionstatechange = null;
          peer.onsignalingstatechange = null;
          peer.oniceconnectionstatechange = null;
          peer.close();
        } catch {
          // ignore
        }
      }
      clearAudio();
      remoteTrackIdsRef.current.clear();
      setRemoteTrackCount(0);
      setConnectionState("closed");
      setSignalingState("closed");
      setIceConnectionState("closed");
      setConnectState((prev) => (prev === "remote_closed" ? "remote_closed" : "closed"));
    },
    [clearAudio],
  );

  const disconnect = React.useCallback(
    async (sendBye: boolean) => {
      const peer = peerRef.current;
      peerRef.current = null;
      closePeerLocal(peer);
      if (sendBye && args.canQuery && args.selectedSessionId) {
        try {
          await args.sendSignalDirect("bye", { reason: "webui_disconnect" });
        } catch (err) {
          setWebrtcError(String(err));
        }
      }
    },
    [args, closePeerLocal],
  );

  const processSignalEvent = React.useCallback(
    async (ev: BrokerAudioSignalEvent) => {
      const peer = peerRef.current;
      if (!peer) return;
      const from = String(ev.from || "").trim().toLowerCase();
      if (from === "webui") return;

      if (ev.type === "answer") {
        const desc = parseSessionDescription("answer", ev.payload);
        if (!desc) return;
        await peer.setRemoteDescription(desc);
        setLastRemoteSignal("answer");
        return;
      }

      if (ev.type === "candidate") {
        const candidate = parseIceCandidate(ev.payload);
        if (!candidate) return;
        await peer.addIceCandidate(candidate);
        setReceivedCandidateCount((prev) => prev + 1);
        setLastRemoteSignal("candidate");
        return;
      }

      if (ev.type === "bye") {
        setLastRemoteSignal("bye");
        setConnectState("remote_closed");
        await disconnect(false);
      }
    },
    [disconnect],
  );

  React.useEffect(() => {
    const peer = peerRef.current;
    peerRef.current = null;
    closePeerLocal(peer);
    handledSignalsRef.current.clear();
    remoteTrackIdsRef.current.clear();
    setRemoteTrackCount(0);
    setSentCandidateCount(0);
    setReceivedCandidateCount(0);
    setLastRemoteSignal("");
    setWebrtcError(null);
    setConnectPending(false);
    setConnectState("idle");
    setConnectionState("new");
    setSignalingState("stable");
    setIceConnectionState("new");
  }, [args.selectedSessionId, closePeerLocal]);

  React.useEffect(() => {
    if (!peerRef.current || args.signalEvents.length === 0) return;
    const run = async () => {
      for (const ev of signalEventsRef.current) {
        const key = makeSignalKey(ev);
        if (handledSignalsRef.current.has(key)) continue;
        handledSignalsRef.current.add(key);
        try {
          await processSignalEvent(ev);
        } catch (err) {
          setWebrtcError(String(err));
        }
      }
    };
    void run();
  }, [args.signalEvents, processSignalEvent]);

  React.useEffect(() => {
    return () => {
      const peer = peerRef.current;
      peerRef.current = null;
      closePeerLocal(peer);
    };
  }, [closePeerLocal]);

  const connect = React.useCallback(async () => {
    if (!supported) {
      setWebrtcError("RTCPeerConnection is not available in this browser");
      return;
    }
    if (!args.canQuery || !args.selectedSessionId) {
      setWebrtcError("select a session first");
      return;
    }
    setConnectPending(true);
    setWebrtcError(null);
    handledSignalsRef.current.clear();
    remoteTrackIdsRef.current.clear();
    setRemoteTrackCount(0);
    setSentCandidateCount(0);
    setReceivedCandidateCount(0);
    setLastRemoteSignal("");
    const peer = peerRef.current;
    peerRef.current = null;
    closePeerLocal(peer);

    try {
      const rtcWindow = window as WebRtcWindow;
      const peer = typeof rtcWindow.__agentuiRtcFactory === "function" ? rtcWindow.__agentuiRtcFactory() : new RTCPeerConnection();
      peerRef.current = peer;
      setConnectState("connecting");
      setConnectionState(peer.connectionState || "new");
      setSignalingState(peer.signalingState || "stable");
      setIceConnectionState(peer.iceConnectionState || "new");

      peer.onconnectionstatechange = () => {
        const next = peer.connectionState || "new";
        setConnectionState(next);
        if (next === "connected") setConnectState("connected");
        else if (next === "closed") setConnectState("closed");
      };
      peer.onsignalingstatechange = () => {
        setSignalingState(peer.signalingState || "stable");
      };
      peer.oniceconnectionstatechange = () => {
        setIceConnectionState(peer.iceConnectionState || "new");
      };
      peer.onicecandidate = (event) => {
        const candidate = event.candidate;
        if (!candidate) return;
        const payload = typeof candidate.toJSON === "function" ? candidate.toJSON() : {
          candidate: candidate.candidate,
          sdpMid: candidate.sdpMid ?? undefined,
          sdpMLineIndex: candidate.sdpMLineIndex ?? undefined,
          usernameFragment: candidate.usernameFragment ?? undefined,
        };
        setSentCandidateCount((prev) => prev + 1);
        void args.sendSignalDirect("candidate", payload as unknown as Record<string, unknown>).catch((err) => setWebrtcError(String(err)));
      };
      peer.ontrack = (event) => {
        const stream = event.streams && event.streams[0] ? event.streams[0] : null;
        const trackId = typeof event.track?.id === "string" ? event.track.id : "";
        if (trackId && !remoteTrackIdsRef.current.has(trackId)) {
          remoteTrackIdsRef.current.add(trackId);
          setRemoteTrackCount(remoteTrackIdsRef.current.size);
        }
        const el = audioRef.current;
        if (el && stream) {
          try {
            el.srcObject = stream;
          } catch {
            // ignore
          }
          const maybePlay = el.play();
          if (maybePlay && typeof maybePlay.catch === "function") {
            void maybePlay.catch(() => {});
          }
        }
      };

      try {
        peer.addTransceiver("audio", { direction: "recvonly" });
      } catch {
        // Some test doubles or browsers may not implement addTransceiver.
      }

      const offer = await peer.createOffer();
      await peer.setLocalDescription(offer);
      setSignalingState(peer.signalingState || "have-local-offer");
      await args.sendSignalDirect("offer", {
        type: offer.type,
        sdp: offer.sdp || "",
      });

      for (const ev of args.signalEvents) {
        const key = makeSignalKey(ev);
        if (handledSignalsRef.current.has(key)) continue;
        handledSignalsRef.current.add(key);
        await processSignalEvent(ev);
      }
    } catch (err) {
      setWebrtcError(String(err));
      const current = peerRef.current;
      peerRef.current = null;
      closePeerLocal(current);
    } finally {
      setConnectPending(false);
    }
  }, [args, closePeerLocal, processSignalEvent, supported]);

  return {
    audioRef,
    connect,
    connectPending,
    connectState,
    connectionState,
    disconnect: React.useCallback(() => void disconnect(true), [disconnect]),
    iceConnectionState,
    lastRemoteSignal,
    receivedCandidateCount,
    remoteTrackCount,
    sentCandidateCount,
    signalingState,
    supported,
    webrtcError,
  };
}
