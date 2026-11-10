// Each call owns these processors through its AudioContexts.
class Capture extends AudioWorkletProcessor {
  constructor() { super(); this.frame = new ArrayBuffer(640); this.view = new DataView(this.frame); this.index = 0; }
  process(inputs, outputs) {
    for (const channel of outputs[0] || []) channel.fill(0);
    const input = inputs[0]?.[0];
    if (input) for (const value of input) {
      this.view.setInt16(this.index * 2, Math.max(-32768, Math.min(32767, Math.round(value * 32768))), true);
      if (++this.index === 320) {
        this.port.postMessage(this.frame, [this.frame]);
        this.frame = new ArrayBuffer(640); this.view = new DataView(this.frame); this.index = 0;
      }
    }
    return true;
  }
}
class Playback extends AudioWorkletProcessor {
  constructor() {
    super(); this.ring = new Int16Array(4 * 1024 * 1024); this.read = 0; this.write = 0; this.count = 0; this.pending = null; this.playing = false;
    this.port.onmessage = ({ data }) => {
      if (data.type === 'reset') { this.read = this.write = this.count = 0; this.pending = null; if (this.playing) this.port.postMessage('drained'); this.playing = false; return; }
      const bytes = new Uint8Array(data.bytes); const samples = Math.floor((bytes.length + (this.pending === null ? 0 : 1)) / 2);
      if (samples > this.ring.length - this.count) { this.port.postMessage('overflow'); return; }
      let i = 0; const push = (value) => { this.ring[this.write] = value; this.write = (this.write + 1) % this.ring.length; this.count++; };
      if (this.pending !== null && bytes.length) { push(this.pending | (bytes[0] << 8)); this.pending = null; i = 1; }
      const view = new DataView(bytes.buffer);
      for (; i + 1 < bytes.length; i += 2) push(view.getInt16(i, true));
      if (i < bytes.length) this.pending = bytes[i];
      if (this.count && !this.playing) { this.playing = true; this.port.postMessage('playing'); }
    };
  }
  process(inputs, outputs) {
    const output = outputs[0]?.[0]; if (!output) return true;
    for (let i = 0; i < output.length; i++) {
      output[i] = this.count ? this.ring[this.read] / 32768 : 0;
      if (this.count) { this.read = (this.read + 1) % this.ring.length; this.count--; }
    }
    if (!this.count && this.playing) { this.playing = false; this.port.postMessage('drained'); }
    return true;
  }
}
registerProcessor('agentc-capture', Capture);
registerProcessor('agentc-playback', Playback);
