import { expect, test, type Page, type WebSocketRoute } from "@playwright/test";

test.use({ launchOptions: { executablePath: process.env.AGENT_E2E_BROWSER_EXECUTABLE, args: ["--use-fake-ui-for-media-stream", "--use-fake-device-for-media-stream", "--autoplay-policy=no-user-gesture-required"] } });

async function workspace(page: Page, delayMicrophone = false) {
  await page.addInitScript(({ delay }) => {
    localStorage.setItem("agentui.connectionMode", JSON.stringify("direct"));
    localStorage.setItem("agentui.base", "http://127.0.0.1:7777");
    localStorage.setItem("agentui.showSettings", "false");
    const state = (window as any).__voiceTest = { tracks: [], contexts: [], release: null };
    const media = navigator.mediaDevices.getUserMedia.bind(navigator.mediaDevices);
    navigator.mediaDevices.getUserMedia = async (options) => {
      const stream = await media(options); state.tracks.push(...stream.getTracks());
      if (delay && options.audio) await new Promise<void>((resolve) => { state.release = resolve; });
      return stream;
    };
    const Original = window.AudioContext;
    window.AudioContext = class extends Original { constructor(options?: AudioContextOptions) { super(options); state.contexts.push(this); } };
  }, { delay: delayMicrophone });
  await page.route("**/api/v1/**", (route) => route.fulfill({ headers: { "access-control-allow-origin": "*", "access-control-allow-headers": "authorization,content-type", "access-control-allow-methods": "GET,POST,OPTIONS" }, json: { ok: true, service: "agentd", sessions: ["default"], messages: [], events: [], runs: [], entries: [], scene: {}, prefs: {}, features: {} } }));
  await page.route("**/voice/v1/config", (route) => route.fulfill({ json: { available: true, model: "qwen3.5-omni-flash-realtime", max_session_seconds: 300 } }));
  const events: any[] = []; const sockets: WebSocketRoute[] = []; let closed = 0;
  await page.routeWebSocket("**/voice/v1/realtime", (socket) => {
    sockets.push(socket); socket.onMessage((raw) => events.push(JSON.parse(String(raw)))); socket.onClose(() => closed++);
    socket.send(JSON.stringify({ type: "session.created" })); socket.send(JSON.stringify({ type: "session.updated" }));
  });
  await page.goto("/"); await page.getByRole("navigation", { name: "Workspace navigation" }).getByRole("button", { name: "Voice", exact: true }).click();
  await expect(page.getByTestId("voice-call-start")).toBeEnabled();
  return { events, sockets, closed: () => closed };
}
async function disposed(page: Page) {
  await expect.poll(() => page.evaluate(() => {
    const state = (window as any).__voiceTest;
    return state.tracks.every((track: MediaStreamTrack) => track.readyState === "ended") && state.contexts.every((context: AudioContext) => context.state === "closed");
  })).toBe(true);
}

for (const width of [1440, 390]) test(`voice streams bounded PCM and images with one caption at ${width}px`, async ({ page }, info) => {
  await page.setViewportSize({ width, height: 900 }); const mock = await workspace(page);
  expect(mock.sockets).toHaveLength(0);
  await page.getByTestId("voice-call-start").click();
  await expect(page.getByTestId("voice-panel").getByRole("status")).toHaveText("Listening");
  await expect.poll(() => mock.events.filter((event) => event.type === "input_audio_buffer.append").length).toBeGreaterThan(2);
  const audio = mock.events.find((event) => event.type === "input_audio_buffer.append"); expect(Buffer.from(audio.audio, "base64").length).toBe(640);
  const socket = mock.sockets[0];
  for (const event of [
    { type: "conversation.item.input_audio_transcription.delta", item_id: "u1", text: "Read this", stash: " image" },
    { type: "conversation.item.input_audio_transcription.completed", item_id: "u1", transcript: "Read this image" },
    { type: "response.created", response: { id: "r1" } },
    { type: "response.text.delta", response_id: "r1", delta: "Ready for the next shift." },
    { type: "response.audio_transcript.delta", response_id: "r1", delta: "Ready for the next shift." },
    { type: "response.audio.delta", response_id: "r1", delta: Buffer.alloc(4800).toString("base64") },
    { type: "response.done", response: { id: "r1", status: "completed" } },
  ]) socket.send(JSON.stringify(event));
  await expect(page.getByTestId("chat-user")).toHaveCount(1); await expect(page.getByTestId("chat-assistant")).toHaveCount(1);
  await expect(page.getByTestId("chat-assistant")).toContainText("Ready for the next shift.");
  await page.getByRole("button", { name: "Mute", exact: true }).click();
  expect(await page.evaluate(() => (window as any).__voiceTest.tracks.filter((track: MediaStreamTrack) => track.kind === "audio").every((track: MediaStreamTrack) => !track.enabled))).toBe(true);
  await page.getByRole("button", { name: "Unmute", exact: true }).click();
  const png = await page.evaluate(() => { const canvas = document.createElement("canvas"); canvas.width = canvas.height = 16; const context = canvas.getContext("2d")!; context.fillStyle = "white"; context.fillRect(0, 0, 16, 16); return canvas.toDataURL("image/png").split(",")[1]; });
  await page.getByTestId("voice-image-input").setInputFiles({ name: "dot.png", mimeType: "image/png", buffer: Buffer.from(png, "base64") });
  await expect.poll(() => mock.events.filter((event) => event.type === "input_image_buffer.append").length).toBeGreaterThan(0);
  const image = mock.events.find((event) => event.type === "input_image_buffer.append"); expect(Buffer.from(image.image, "base64").subarray(0, 2)).toEqual(Buffer.from([255, 216]));
  expect(image.image.length).toBeLessThanOrEqual(256 * 1024);
  await expect.poll(() => mock.events.filter((event) => event.type === "input_image_buffer.append").length).toBeGreaterThan(1);
  await page.getByRole("button", { name: "Remove image", exact: true }).click();
  await expect(page.getByAltText("Shared image")).toHaveCount(0);
  const beforeCamera = mock.events.filter((event) => event.type === "input_image_buffer.append").length;
  await page.getByRole("button", { name: "Share camera", exact: true }).click();
  await expect(page.getByRole("button", { name: "Stop camera", exact: true })).toBeVisible();
  await expect.poll(() => mock.events.filter((event) => event.type === "input_image_buffer.append").length).toBeGreaterThan(beforeCamera);
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
  await page.screenshot({ path: info.outputPath("omni-voice.png") });
  await page.getByTestId("voice-call-end").click(); await disposed(page); await expect.poll(mock.closed).toBe(1);
  await expect(page.getByTestId("voice-panel").getByRole("status")).toHaveText("Call ended");
});

test("ending a call while microphone permission is pending releases late media", async ({ page }) => {
  const mock = await workspace(page, true); await page.getByTestId("voice-call-start").click();
  await expect.poll(() => page.evaluate(() => Boolean((window as any).__voiceTest.release))).toBe(true);
  await page.getByTestId("voice-call-end").click(); await page.evaluate(() => (window as any).__voiceTest.release());
  await disposed(page); expect(mock.sockets).toHaveLength(0);
  await expect(page.getByTestId("voice-panel").getByRole("status")).toHaveText("Call ended");
});

test("leaving voice closes its socket and microphone", async ({ page }) => {
  const mock = await workspace(page); await page.getByTestId("voice-call-start").click();
  await expect.poll(() => mock.events.length).toBeGreaterThan(0);
  await page.getByRole("navigation", { name: "Workspace navigation" }).getByRole("button", { name: "Chat", exact: true }).click();
  await disposed(page); await expect.poll(mock.closed).toBe(1);
});

test("provider failure ends capture and exposes a concise error", async ({ page }) => {
  const mock = await workspace(page); await page.getByTestId("voice-call-start").click();
  await expect.poll(() => mock.events.length).toBeGreaterThan(0);
  mock.sockets[0].send(JSON.stringify({ type: "error", error: { code: "model_unavailable", message: "Voice is temporarily unavailable." } }));
  await expect(page.getByRole("alert")).toHaveText("Voice is temporarily unavailable."); await disposed(page); await expect.poll(mock.closed).toBe(1);
  await expect(page.getByTestId("voice-call-start")).toBeEnabled();
});
