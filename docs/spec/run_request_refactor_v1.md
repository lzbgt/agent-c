# run_request.cpp Refactor Plan (v1)

Date: 2026-02-19

## Goals
- Reduce `daemon/src/run_request.cpp` size and complexity without behavior changes.
- Isolate cohesive responsibilities (parsing, provider config, tool-loop execution, replay bundle) into testable units.
- Keep public API and JSON contract stable.

## Non-goals
- No changes to response schema, error messages, or HTTP status codes.
- No provider/tool-loop policy changes.
- No behavioral changes to session persistence or memory logic.

## Constraints
- Rolling style; backward compatibility not required, but avoid churn for stable APIs.
- Minimize merge risk: each extraction should be small and compile-clean.

## Proposed Module Split
1) **Run replay bundle**
   - New module for redaction + capped JSON + replay hash.
   - Status: implemented in `run_request_replay.{h,cpp}`.
2) **Request parsing / validation**
   - Extract JSON parsing, trace-id validation, tool config normalization into a helper module.
   - Status: implemented in `run_request_parse.{h,cpp}`.
3) **Provider config building**
   - Consolidate OpenAI-compatible config overrides + retry policy into a dedicated builder.
   - Status: implemented in `run_request_config.{h,cpp}`.
4) **Tool-loop execution**
   - Wrap toolset selection + tool-loop invocation into a smaller executor function.
5) **Persistence block**
   - Move DB writes (runs/events/messages/tool records) into a persistence helper to reduce side-effects in the main flow.
   - Status: implemented in `run_request_persist.{h,cpp}`.

## Rollout Plan
- **Phase 1**: Extract replay bundle logic (done).
- **Phase 2**: Extract provider config builder + request parsing helpers (done).
- **Phase 3**: Extract persistence helpers (done).
- **Phase 4**: Extract tool-loop execution wrapper and simplify the main orchestration function (pending).

## Test Strategy
- Keep existing smoke tests and CTest suite; add targeted unit tests if new helpers have logic.
- Ensure `tools/verify.sh --skip-ui` remains green after each phase.
