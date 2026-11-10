export function encodeBase64(bytes: Uint8Array): string {
  let text = "";
  for (let i = 0; i < bytes.length; i += 8192) text += String.fromCharCode(...bytes.subarray(i, i + 8192));
  return btoa(text);
}

export class VoiceAudio {
  private input: AudioContext;
  private output: AudioContext;
  private microphone?: MediaStream;
  private source?: MediaStreamAudioSourceNode;
  private capture?: AudioWorkletNode;
  private playback?: AudioWorkletNode;
  private gain?: GainNode;
  private closed = false;

  constructor(private onAudio: (bytes: Uint8Array) => void, private onPlaying: (playing: boolean) => void, private onError: (message: string) => void) {
    this.input = new AudioContext({ sampleRate: 16000 });
    try { this.output = new AudioContext({ sampleRate: 24000 }); }
    catch (error) { void this.input.close().catch(() => {}); throw error; }
  }
  async start() {
    const media = navigator.mediaDevices.getUserMedia({ audio: { channelCount: 1, echoCancellation: true, noiseSuppression: true }, video: false }).then((stream) => {
      if (this.closed) { stream.getTracks().forEach((track) => track.stop()); throw new DOMException("Call ended", "AbortError"); }
      this.microphone = stream; return stream;
    });
    const module = new URL("voice-audio.js", document.baseURI).href;
    const [stream] = await Promise.all([media, this.input.resume(), this.output.resume(), this.input.audioWorklet.addModule(module), this.output.audioWorklet.addModule(module)]);
    if (this.closed) return;
    this.capture = new AudioWorkletNode(this.input, "agentc-capture", { channelCount: 1, numberOfInputs: 1, numberOfOutputs: 1 });
    this.capture.port.onmessage = ({ data }) => { if (!this.closed) this.onAudio(new Uint8Array(data)); };
    this.source = this.input.createMediaStreamSource(stream);
    this.source.connect(this.capture); this.capture.connect(this.input.destination);
    this.playback = new AudioWorkletNode(this.output, "agentc-playback", { numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [1] });
    this.playback.port.onmessage = ({ data }) => {
      if (this.closed) return;
      if (data === "overflow") this.onError("Voice playback could not keep up.");
      else this.onPlaying(data === "playing");
    };
    this.gain = this.output.createGain(); this.playback.connect(this.gain); this.gain.connect(this.output.destination);
  }
  play(base64: string) {
    if (this.closed || !this.playback) return;
    const text = atob(base64); const bytes = Uint8Array.from(text, (character) => character.charCodeAt(0));
    this.playback.port.postMessage({ type: "audio", bytes: bytes.buffer }, [bytes.buffer]);
  }
  resetPlayback() { this.playback?.port.postMessage({ type: "reset" }); }
  muteMicrophone(muted: boolean) { this.microphone?.getAudioTracks().forEach((track) => { track.enabled = !muted; }); }
  muteSpeaker(muted: boolean) { if (this.gain) this.gain.gain.value = muted ? 0 : 1; }
  close() {
    if (this.closed) return; this.closed = true;
    this.microphone?.getTracks().forEach((track) => track.stop());
    this.source?.disconnect(); this.capture?.disconnect(); this.playback?.disconnect(); this.gain?.disconnect();
    void this.input.close().catch(() => {}); void this.output.close().catch(() => {});
  }
}

export async function imageJPEG(source: CanvasImageSource, width: number, height: number): Promise<string> {
  const ratio = Math.min(1, 1280 / Math.max(width, height), 720 / Math.min(width, height));
  const canvas = document.createElement("canvas");
  canvas.width = Math.max(1, Math.round(width * ratio)); canvas.height = Math.max(1, Math.round(height * ratio));
  const context = canvas.getContext("2d"); if (!context) throw new Error("Image sharing is unavailable.");
  context.drawImage(source, 0, 0, canvas.width, canvas.height);
  for (const quality of [0.72, 0.5, 0.3]) {
    const blob = await new Promise<Blob | null>((resolve) => canvas.toBlob(resolve, "image/jpeg", quality));
    if (blob && blob.size <= 190 * 1024) return encodeBase64(new Uint8Array(await blob.arrayBuffer()));
  }
  throw new Error("This image is too detailed to share. Choose a smaller image.");
}
