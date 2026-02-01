# ESP32-S3 Example: `agent_core` as an ESP-IDF component (skeleton)

This is a **starting point** for running the repo’s embedded-safe `agent_core` on ESP32-S3 using ESP-IDF.

It is intentionally minimal:
- no Wi‑Fi / TLS / LLM provider implementation here
- focuses on wiring `agent_core` + a tool executor

Related design docs:
- `docs/EMBEDDED_C_API.md`
- `docs/ESP32S3_AGENT_CORE_MATURITY.md`

## Layout

- `components/agent_core/` — ESP-IDF component wrapper that compiles the repo’s `core/src/*.c`
- `main/` — application entry point (`app_main.c`) with a fake provider + example tool executor

## Build prerequisites

- ESP-IDF installed (`idf.py` available)
- ESP32-S3 toolchain configured

## Quickstart (template)

From this directory:

```sh
idf.py set-target esp32s3
idf.py build
idf.py flash monitor
```

## Next steps

1) Replace the fake provider with a real tool-capable provider:
- implement `agent_tool_provider_t.generate` using `esp_http_client` + a JSON parser (e.g. cJSON)

2) Implement your actual peripheral tools:
- register tools via `agent_tool_registry_add`
- execute tools via `agent_tool_executor_t.execute`

3) Add persistence:
- implement `agent_persistor_t` using NVS or a filesystem and `agent_session_codec_encode_v1`

