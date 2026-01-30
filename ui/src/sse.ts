export type SseEvent = {
  event: string;
  data: string;
  id?: string;
};

function parseSseBlock(block: string): SseEvent | null {
  const lines = block.replace(/\r/g, "").split("\n");
  let event = "message";
  let id: string | undefined;
  const dataLines: string[] = [];

  for (const rawLine of lines) {
    const line = rawLine.trimEnd();
    if (line.length === 0) continue;
    if (line.startsWith(":")) {
      // Comment / ping.
      continue;
    }
    const idx = line.indexOf(":");
    if (idx < 0) continue;
    const k = line.slice(0, idx).trim();
    const v = line.slice(idx + 1).trimStart();
    if (k === "event") event = v;
    else if (k === "id") id = v;
    else if (k === "data") dataLines.push(v);
  }

  if (dataLines.length === 0 && event === "message" && !id) return null;
  return { event, data: dataLines.join("\n"), id };
}

export async function readSseStream(
  resp: Response,
  onEvent: (ev: SseEvent) => void,
): Promise<void> {
  if (!resp.body) {
    throw new Error("response has no body");
  }
  const reader = resp.body.getReader();
  const decoder = new TextDecoder();
  let buf = "";

  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    buf += decoder.decode(value, { stream: true });
    for (;;) {
      const sep = buf.indexOf("\n\n");
      if (sep < 0) break;
      const block = buf.slice(0, sep);
      buf = buf.slice(sep + 2);
      const ev = parseSseBlock(block);
      if (ev) onEvent(ev);
    }
  }
}

