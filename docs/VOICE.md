# Realtime voice and images

The Voice workspace defaults to **Aliyun Omni**, using `qwen3.5-omni-flash-realtime` and the `Ethan` voice. Start voice call begins microphone capture; mute, speaker, camera, image sharing and End call are the primary controls. Captions reuse the chat renderer, with one message per utterance/reply even when the provider sends both text and speech transcripts. Other workspace work continues independently.

Microphone and camera capture begin from explicit user actions, subject to the browser's normal device permissions. Ending a call, leaving Voice or changing the conversation closes its WebSocket, AudioContexts, tracks, image capture and timers. Late permission results are disposed too. Voice captions belong to the current call and are not saved in the daemon's conversation/work ledger. This voice adapter has no workspace execution tools; an audio reply is not evidence of an executed agent action.

## Deployment and credentials

The Arch installer configures the gateway's `voice` section:

```json
{
  "voice": {
    "env_file": "/home/bruce/.env",
    "endpoint": "wss://dashscope.aliyuncs.com/api-ws/v1/realtime",
    "model": "qwen3.5-omni-flash-realtime",
    "voice": "Ethan"
  }
}
```

The owner-only regular env file supplies `ALIYUN_MODEL_STUDIO_AK`, or `DASHSCOPE_API_KEY`. It is read as data without executing shell expressions. The gateway holds the key and sends it only in the provider's WebSocket Authorization header. The browser receives neither provider credentials nor an endpoint containing a key. Missing/invalid voice configuration makes Voice unavailable while the rest of the workspace remains usable. Restart the gateway after changing its credential/configuration file.

Workspace authentication and administrator access protect `GET /voice/v1/config` and the same-origin WebSocket `/voice/v1/realtime`. Long-lived connections recheck access every 15 seconds and close on revocation or identity-service failure. Calls are limited to five minutes, one per account and four per gateway instance. Shared OAuth cookies and gateway operator tokens never reach Aliyun.

The verified model uses the classic Beijing endpoint. The international classic endpoint is supported for a matching key/account. Qwen3.8 requires an appropriate workspace endpoint and credential; changing a model name alone is not a qualification. Defaults follow the inspected production `~/work/lang_learn` reference on the Mac and the actual Model Studio account handshake/inference results.

## Streaming contract

The gateway owns `session.update` and waits for `session.updated` before accepting media. It pins PCM input/output, text/audio output, input ASR, server VAD and a 2,048-token response cap. Browser events cannot override session settings, tools or provider authentication.

- Input is mono signed PCM16LE at 16 kHz, normally 640-byte/20 ms frames. Output is mono PCM16LE at 24 kHz.
- AudioWorklets own capture/playback; an 8 MiB ring bounds queued speaker PCM. Interrupted replies flush local playback and cancel the active response.
- Images become JPEG at up to 1280×720, with at most 190 KiB raw/256 KiB Base64 per frame. A first audio frame precedes images. Images commit with the provider's audio turn, not as independent text requests.
- Camera frames and the currently shared still image refresh at approximately one frame per second. Still-image refresh stops on Remove image, call end or navigation. Camera sharing takes precedence while active.
- The gateway validates event fields, PCM framing, JPEG headers/dimensions, media rates and message sizes. It allows at most 100 input events/second with a bounded burst, 128 KiB/second PCM with a 256 KiB burst, and one image/second with a two-image burst. Per-message audio is at most 64 KiB; browser input/server output messages are capped at 384 KiB/1 MiB.

Browser device permissions and HTTPS/localhost are required for microphone/camera capture. Synthetic browser tests establish lifecycle/format behavior; physical device acoustics, echo cancellation, mobile backgrounding and human listening quality require separate device checks.

## Reproduce qualification

`tools/verify_current_snapshot.sh` includes fake-media browser checks and mock-provider Go tests without live credentials. `cd broker && go test -race ./...` checks the gateway's concurrent cleanup, access revocation and capacity behavior.

For an opt-in live test, prepare synthetic fixtures with a local speech synthesizer, FFmpeg and ImageMagick (these are qualification dependencies, not production dependencies):

```sh
mkdir -p build
espeak-ng -s 145 -w build/voice_fixture.wav \
  'Read the word in the image. Reply with that word only.'
ffmpeg -hide_banner -loglevel error -y -i build/voice_fixture.wav \
  -ar 16000 -ac 1 -f s16le build/voice_fixture.pcm
magick -size 640x360 xc:white -fill black -gravity center \
  -font /path/to/an/installed/font.ttf -pointsize 100 \
  -annotate 0 READY build/voice_fixture.jpg
cd broker
go run ./cmd/agentc-voice-check \
  -audio ../build/voice_fixture.pcm -image ../build/voice_fixture.jpg
```

The checker reads the private deployed gateway config, contacts its public authenticated WebSocket, and verifies ASR, image understanding, reply captions and nonempty PCM output within 55 seconds. It prints only results and the synthetic fixture captions. `-config` selects another private config; `-gateway http://127.0.0.1:PORT` permits a local gateway qualification. The model receives synthetic fixtures, and the check incurs a bounded provider request. It does not access a real microphone or camera.

## Primary references

Inspected 2026-11-10: [Aliyun WebSocket access](https://www.alibabacloud.com/help/en/model-studio/omni-realtime-websocket-access), [client events and image/audio limits](https://www.alibabacloud.com/help/en/model-studio/client-events), and [Omni Realtime API](https://www.alibabacloud.com/help/en/model-studio/omni-realtime-api). The Mac reference contributed the server-held key pattern, 16/24 kHz PCM formats, bounded playback and verified 3.5 Flash/Ethan defaults. Repository implementation remains self-contained; reproducing it does not require SSH access to that project.
