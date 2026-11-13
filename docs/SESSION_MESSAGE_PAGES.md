# Generation-fenced session message pages

This is bounded access to the current canonical conversation snapshot, **not append-only history**. Context compaction, replacement or deletion can remove messages. Message indices are meaningful only together with the session ID and returned revision. They are not permanent message IDs.

## Read the newest page

`GET /api/v1/session?session_id=example&messages_limit=100`

Normal daemon authentication applies. The response retains `ok`, `session_id` and `messages`. Each message adds its zero-based `index`. Messages within a page are chronological (ascending index); the first page selects the newest messages.

`page` contains:
- `revision`: durable conversation generation.
- `has_more`: whether older rows remain.
- `next_before_index`: exclusive cursor for older rows, or null at the end.

Continue using both the returned cursor and unchanged revision:

`GET /api/v1/session?session_id=example&messages_limit=100&messages_before_index=400&messages_revision=12`

Prepend each older page. Never concatenate a page from a different revision. A 409 means the conversation changed; discard accumulated pages and restart. This includes normal writes and deletion/recreation. Each page reads existence, revision and rows inside one SQLite read transaction, including when another connection writes concurrently. Restart alone does not invalidate a cursor.

## Bounds and errors

- 1–200 messages per request; no offset scan or whole-history materialization.
- Raw role/content/media text is capped at 2 MiB per page. JSON encoding adds bounded overhead.
- A page may contain fewer rows because of the byte cap and still have a continuation.
- A single oversized message returns 413 rather than silent truncation. The legacy complete session export remains available for deliberate retrieval.
- Invalid/fractional/negative/unsafe-integer cursors return 400. A continuation requires a revision.
- Missing/deleted session: 404. Authentication remains mandatory.
- Without any paging parameters, the legacy complete response is unchanged.

The TypeScript `apiGetSessionPage` helper validates bounds, returned session/revision, ordering and continuation identity. It accepts an AbortSignal and raises `SessionPageError` with HTTP status, including 409. It does not silently fall back to the unbounded endpoint or merge stale pages. Existing UI history remains on its previous retrieval path; this source milestone exposes the native and typed-client paging capability, not a claim that every UI consumer has migrated.

## Verification

`python3 tests/session_message_pages_smoke.py build/agentd` exercises a temporary authenticated daemon: 501-message traversal without gaps or duplicates, restart, legacy compatibility, media, strict parameters, row/byte caps, normal append, deletion/recreation, and concurrent second-connection commits.

`ui/e2e/session_message_pages.spec.ts` exercises the actual typed browser module and rejects stale identities or invalid ordering. No production service or real conversation is used.
