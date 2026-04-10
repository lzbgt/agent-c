# Native Voice Media Stack Research

Date: 2026-04-11

## Goal

Establish the factual local baseline for replacing the builtin `native_plugin`
 mock provider with a real embedded/native media engine behind
`daemon/src/session_voice_builtin_media_engine.cpp`.

## Commands Run

```bash
pkg-config --list-all | rg 'opus|srtp|webrtc|datachannel|portaudio|pulse'
pkg-config --modversion opus portaudio-2.0
ls /opt/homebrew/include /opt/homebrew/lib | rg 'opus|srtp|webrtc|datachannel|portaudio|pulse'
brew --version
brew info libdatachannel srtp libusrsctp libjuice
HOMEBREW_NO_AUTO_UPDATE=1 brew install srtp libusrsctp libjuice
python3 tools/check_voice_native_media_stack.py --pretty
python3 tools/inspect_voice_media_provider.py build/libagentd_voice_builtin_media_engine_embedded_transport.so --pretty
rg -n "libdatachannel|libwebrtc|libsrtp|srtp|opus|portaudio|pulse|webrtc::|rtc::|RTP|SRTP|SDP" \
  daemon CMakeLists.txt tests docs tools --glob '!tests/*.sh' --glob '!tests/*.js'
```

Reference artifacts downloaded into the repo for exact vendor details:

- `docs/research/vendor-libsrtp-README-20260411.md`
- `docs/research/vendor-libusrsctp-Manual-20260411.md`
- `docs/research/vendor-libjuice-README-20260411.md`
- `docs/research/vendor-libjuice-connectivity-20260411.c`

## Local Machine Findings

- `pkg-config` reports locally installed audio primitives:
  - `opus 1.5.2`
  - `portaudio 19`
- After installing the transport dependencies, the current local machine now has:
  - `libsrtp2 2.8.0`
  - `libusrsctp 0.9.5.0`
  - `libjuice 1.7.0`
- `tools/check_voice_native_media_stack.py --pretty` now reports:
  - `audio_primitives_ready=true`
  - `transport_primitives_ready=true`
  - `embedded_webrtc_candidate_ready=true`
- The exact local install surfaces are now:
  - `libsrtp2` via `pkg-config` name `libsrtp2` and filesystem path
    `/opt/homebrew/opt/srtp/include/srtp2/srtp.h`
  - `libusrsctp` via `pkg-config` name `usrsctp` and filesystem path
    `/opt/homebrew/opt/libusrsctp/include/usrsctp.h`
  - `libjuice` via filesystem detection only
    (`/opt/homebrew/opt/libjuice/include/juice/juice.h`,
    `/opt/homebrew/opt/libjuice/lib/libjuice.dylib`)
- Packaging caveat confirmed from the installed files:
  - `libjuice` ships without a `pkg-config` file in this environment
  - Homebrew `libusrsctp` ships a broken `pkg-config` include dir
    (`.../include/usrsctp`) even though the actual header is at
    `.../include/usrsctp.h`
- `libdatachannel` still is **not** available as a Homebrew formula in this
  environment, and there is still no installed `libwebrtc`.
- Repo scan across `daemon/`, `CMakeLists.txt`, native tests, and tooling found
  no in-tree integration for:
  - `libdatachannel`
  - `libwebrtc`
  - any equivalent embedded RTP/SRTP/WebRTC engine that already terminates RTP
    or DTLS natively inside agentd

## Repo State At Scan Time

- The builtin runtime already had:
  - `signaling_stub` mode
  - `native_plugin` mode
  - a loadable provider ABI
  - a shipped sample provider target plus ABI-v1 compatibility fixture coverage
- The repo now also has an optional dependency-backed embedded transport
  provider target when the required native libraries are present:
  - `agentd_voice_builtin_media_engine_embedded_transport`
  - provider name `agentd_builtin_embedded_transport_provider`
  - shipped provider ABI is now pollable v4 instead of callback-only v2
  - capabilities:
    - `ice=true`
    - `srtp=true`
    - `sctp=true`
    - `audio_drain=true`
    - `audio_owner_handoff=true`
    - `transport_family=embedded_transport_primitives`
    - `real_media_engine=false`
- The embedded transport provider now also proves a stronger factual boundary
  than "library loads":
  - it starts real `libjuice` candidate gathering before generating the answer
    SDP returned through the provider ABI
  - it now generates an ephemeral local DTLS identity and fingerprint for that
    answer path
  - the returned answer SDP now carries gathered ICE candidates instead of only
    credentials, and mirrors browser-style media offers into an inactive answer
    with `a=setup:passive` plus a surfaced `sha-256` fingerprint
  - direct provider coverage now includes a local libjuice loopback peer that
    exchanges a real offer, consumes the candidate-bearing answer, trickles
    remote candidates back into the provider, and proves post-answer transport
    progression with live libjuice state and candidate counters
- The repo now also has a direct in-tree DTLS/SRTP proof slice independent of
  that synthetic libjuice role quirk:
  - `session_voice_builtin_dtls_transport_tests` completes a DTLS 1.2
    client/server handshake using the same OpenSSL server/client configuration
    the builtin provider now carries
  - that test negotiates `SRTP_AES128_CM_SHA1_80`
  - it also proves `SSL_export_keying_material(..., "EXTRACTOR-dtls_srtp", ...)`
    succeeds, which is the exact prerequisite for SRTP context bring-up
  - it now also derives inbound/outbound libsrtp contexts from that exporter
    output and successfully protects then unprotects a sample RTP packet
- Agentd now also has the matching daemon-side async ingest seam for that
  provider family:
  - builtin media providers can expose a `poll_status` callback through ABI v3
  - the builtin service loop now drains queued provider progress events between
    broker signaling ingress timeouts
  - the embedded provider uses that path to surface async libjuice / DTLS /
    SRTP status progression without waiting for another signaling message
- The repo still does **not** yet have a full embedded/native audio engine.
  The embedded provider now uses real `libjuice` / `libsrtp` / `usrsctp`
  libraries, derives live SRTP contexts from the negotiated DTLS exporter, and
  can terminate inbound SRTP/RTP packets far enough to parse payload-type /
  sequence / timestamp / SSRC metadata. That is why it now truthfully reports
  `native_media_supported=true` for receive-side media ownership.
- The repo now also has a minimal receive-side audio stage instead of only RTP
  termination counters:
  - RTP payload types are mapped from the remote SDP offer
  - `PCMU` and `PCMA` decode directly in-tree
  - `OPUS` decode is now wired through `libopus` when it is present at build
    time
  - decoded PCM samples are staged in-process behind bounded counters/telemetry
  - staged PCM can now be drained through the provider ABI into agentd-owned
    bounded PCM memory, so the daemon itself now owns a minimal receive-side
    audio buffer instead of leaving all PCM ownership inside the provider
- Agentd now also consumes that owned PCM through a bounded in-process monitor
  stage:
  - provider-drained PCM is no longer only counted and retained
  - the builtin service loop now consumes bounded chunks, computes peak/RMS
    telemetry, and persists process counters in the runtime snapshot
  - this makes the daemon a real minimal in-process audio owner rather than
    only a handoff sink between provider staging and status counters
- Agentd now also has a first concrete local render sink for that consumed PCM:
  - the builtin service loop rewrites a bounded rolling `audio_recent.wav`
    snapshot under the runtime directory
  - runtime status now persists render counters plus the current WAV path and
    last render error
  - cleanup remains automatic because the WAV snapshot lives under the same
    runtime-artifact directory already swept by voice-runtime cleanup
- Agentd now also has an opt-in best-effort local playback sink for that
  consumed PCM:
  - `audio_webrtc.builtin_local_playback=true` or
    `AGENTD_AUDIO_WEBRTC_BUILTIN_LOCAL_PLAYBACK=1` mirrors processed PCM to the
    default PortAudio output device
  - runtime status now persists playback counters, queued sample depth, device
    name, and last playback error
- Agentd now also has a first bounded outbound media path:
  - native providers can expose ABI v5 `submit_audio`
  - the builtin service submits processed agentd-owned PCM back to the provider
  - the embedded provider negotiates outbound Opus when `libopus` is present,
    falls back to negotiated G.711 payload type from the remote SDP, encodes a
    20 ms Opus or PCMU/PCMA RTP frame, protects it with the outbound libsrtp
    context, and transmits it over the same libjuice transport
  - runtime status now persists outbound RTP and PCM-submit counters, including
    `rtp_packets_sent`, `rtp_payload_bytes_sent`,
    `audio_outbound_frames_sent`, and
    `audio_pcm_samples_submitted_total`, plus selected outbound
    payload/codec/rate/channel metadata
- The remaining gap is that the outbound path is deliberately minimal: it uses
  single-frame negotiated Opus/G.711 generation rather than fuller
  RTCP/full-duplex behavior.

## Implications

The highest-confidence next implementation path is:

1. Keep the current daemon/provider seam as the stable insertion point.
2. Require future native providers to declare capabilities and ABI metadata.
3. Keep the current dependency-backed embedded provider as the transport
   primitives bring-up lane.
4. Grow the embedded provider's new inbound/decode/PCM-submit/outbound-PCMU
   path into a fuller negotiated in-process DTLS/SRTP/RTP audio engine instead
   of adding more control-plane glue.

At this scan point, the local machine is ready for:

- provider ABI/tooling work
- diagnostics/probe surfacing
- shipped sample-provider bring-up and contract validation
- dependency-backed embedded provider bring-up against `libjuice + libsrtp2 +
  usrsctp`
- optional audio I/O experiments using `opus` / `portaudio`

It is **not** yet at a full embedded WebRTC/SRTP runtime. The remaining gap is
no longer dependency discovery or basic RTP ownership; it is fuller RTCP,
full-duplex, and negotiated Opus media behavior.

## Recommended Next Technical Decision

Pick one concrete native backend family and wire it deliberately:

- `libdatachannel` family:
  - not currently available as a Homebrew formula on this machine
  - would likely require vendoring or manual source integration
- full `libwebrtc` embed:
  - largest integration cost
  - strongest long-term browser/WebRTC parity
- narrower `libjuice + srtp + libusrsctp + opus + portaudio` path:
  - now confirmed locally installed and buildable in-tree
  - already proven through the optional embedded transport provider module
  - still requires explicit DTLS/SRTP/session implementation work in-tree
  - avoids blocking on `libdatachannel` packaging

Given the current repo and machine facts, the most pragmatic next move is to
continue on the existing `native_plugin` seam and harden the current embedded
provider from bounded negotiated Opus/G.711 transmit toward fuller
bidirectional audio.
The best factual candidate remains the narrower `libjuice + srtp + libusrsctp`
family, because those dependencies are now locally installed, buildable, and
covered by provider inspection/unit/smoke proof. The bounded outbound RTP path
now negotiates Opus when `libopus` is present, falls back to G.711 payload type
from the remote SDP (`PCMU/8000/1` preferred, `PCMA/8000/1` fallback), and
records that selection in runtime telemetry. The next concrete step after this
negotiated transmit path is RTCP/full-duplex behavior rather than more
DTLS/SRTP/control-plane work.
