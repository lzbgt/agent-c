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
python3 tools/check_voice_native_media_stack.py --pretty
rg -n "libdatachannel|libwebrtc|libsrtp|srtp|opus|portaudio|pulse|webrtc::|rtc::|RTP|SRTP|SDP" \
  daemon CMakeLists.txt tests docs tools --glob '!tests/*.sh' --glob '!tests/*.js'
```

## Local Machine Findings

- `pkg-config` reports locally installed audio primitives:
  - `opus 1.5.2`
  - `portaudio 19`
- `/opt/homebrew/include` and `/opt/homebrew/lib` also contain:
  - `opus`
  - `portaudio.h`
  - `libopus.*`
  - `libportaudio.*`
- The same scan did **not** find locally installed native WebRTC transport
  pieces:
  - no `libsrtp`
  - no `libdatachannel`
  - no `libwebrtc`
  - no `usrsctp`
- Homebrew formula availability is better than the original quick scan implied:
  - `libjuice` exists in Homebrew Core and was not installed locally
  - `srtp` exists in Homebrew Core and was not installed locally
  - `libusrsctp` exists in Homebrew Core and was not installed locally
  - `libdatachannel` still was **not** available as a Homebrew formula in this
    environment
- Repo scan across `daemon/`, `CMakeLists.txt`, native tests, and tooling found
  no in-tree integration for:
  - `libdatachannel`
  - `libwebrtc`
  - `libsrtp`
  - any equivalent embedded RTP/SRTP/WebRTC engine

## Repo State At Scan Time

- The builtin runtime already had:
  - `signaling_stub` mode
  - `native_plugin` mode
  - a loadable provider ABI
  - a shipped sample provider target plus ABI-v1 compatibility fixture coverage
- The repo did **not** yet have an actual embedded/native RTP media engine.

## Implications

The highest-confidence next implementation path is:

1. Keep the current daemon/provider seam as the stable insertion point.
2. Require future native providers to declare capabilities and ABI metadata.
3. Choose a concrete transport stack explicitly instead of assuming local system
   packages already satisfy it.

At this scan point, the local machine is ready for:

- provider ABI/tooling work
- diagnostics/probe surfacing
- shipped sample-provider bring-up and contract validation
- optional audio I/O experiments using `opus` / `portaudio`

It is **not** yet ready for a real embedded WebRTC/SRTP runtime without adding
new dependencies or vendoring a stack.

## Recommended Next Technical Decision

Pick one concrete native backend family and wire it deliberately:

- `libdatachannel` family:
  - not currently available as a Homebrew formula on this machine
  - would likely require vendoring or manual source integration
- full `libwebrtc` embed:
  - largest integration cost
  - strongest long-term browser/WebRTC parity
- narrower `libjuice + srtp + libusrsctp + opus + portaudio` path:
  - now confirmed possible from local package availability facts
  - still requires explicit DTLS/SRTP/session implementation work in-tree
  - avoids blocking on `libdatachannel` packaging

Given the current repo and machine facts, the most pragmatic next move is to
prototype against a chosen provider/backend family behind the existing
`native_plugin` seam rather than expanding signaling-only behavior further. The
current best factual candidate is the narrower `libjuice + srtp + libusrsctp`
family, because those dependencies are available through Homebrew even though
they are not yet installed locally.
