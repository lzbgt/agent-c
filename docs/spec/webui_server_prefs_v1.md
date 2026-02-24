# WebUI Server-Side Connection Prefs v1

Last updated: 2026-02-24

## Goals

- Persist **WebUI connection profiles** (daemon/broker URLs, agent ids, profile names) on the **daemon or broker** so they survive browser resets and are shared across devices.
- Keep secrets **off** the server by default (auth tokens remain local unless explicitly opted in later).
- Make the flow robust when the daemon is restarted (prefs stored in agentd DB meta table).
- Maintain backward compatibility with existing localStorage-only behavior.

## Non-goals (v1)

- Multi-user account separation beyond auth subject + client_id (no admin cross-user browsing UI yet).
- Synchronizing all WebUI settings (run settings, memory preferences, UI layout).
- Storing auth tokens server-side (explicit opt-in may be added later).

## Constraints

- WebUI is a static site; it cannot bootstrap without an initial base URL.
- Prefs must be **auth-protected** when agentd auth is enabled.
- Data volume is small (profiles list), so a single JSON blob is sufficient.

## Architecture

### Storage (agentd)

- agentd DB `meta` table.
- Key: `client.prefs.<client_kind>.<client_id>`
- Value: JSON blob with `version`, `client_id`, `client_kind`, `prefs`, `updated_utc_ms`.

### API (agentd)

- `GET /api/v1/client/prefs?client_id=...&client_kind=webui`
  - Returns `{ ok, found, prefs, updated_utc_ms }`
- `POST /api/v1/client/prefs`
  - Body: `{ client_id, client_kind, prefs }`
  - Returns `{ ok, prefs, updated_utc_ms }`
- Both require daemon auth when enabled.

### Storage (broker)

- broker DB table `broker_client_prefs`.
- Key: `(owner_sub, client_kind, client_id)`.
- Value: JSON blob with `version`, `client_id`, `client_kind`, `prefs`, `updated_utc_ms`.

### API (broker)

- `GET /v1/client_prefs?client_id=...&client_kind=webui`
  - Returns `{ ok, found, prefs, updated_utc_ms }`
  - OIDC required; uses caller `sub` as owner.
- `POST /v1/client_prefs`
  - Body: `{ client_id, client_kind, prefs }`
  - Returns `{ ok, prefs, updated_utc_ms }`
  - OIDC required; uses caller `sub` as owner.

### Prefs Schema (v1)

```
prefs: {
  connection: {
    active_profile_id: string,
    profiles: [
      { id, name, mode, base, brokerBase, brokerAgentId, brokerDeploymentId }
    ]
  }
}
```

Notes:
- `daemonAuthToken` and `brokerAuthToken` are **never** stored in v1.
- Unknown fields are ignored to allow forward compatibility.

## WebUI Behavior

- Toggle: “Sync connection profiles to daemon”.
- On enable:
  - Pull prefs from daemon (direct mode) or broker (broker mode) and merge with local tokens.
  - Push sanitized profiles (no secrets) on change.
- On failure: keep local settings, surface a warning, and retry on demand.

## Future Enhancements

- Admin/ops tooling to inspect or reset prefs across users.
- Optional encrypted secret storage (tokens).
- Full UI settings sync with versioned migrations.
