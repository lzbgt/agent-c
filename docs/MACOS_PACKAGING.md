# macOS Packaging, Codesign, and Notarization (agentd/agent)

Date: 2026-02-14

This guide covers a **production-grade** packaging pipeline for macOS (Apple Silicon, M2). It produces a signed `.pkg`
containing `agentd` (and optionally `agent`) installed under `/usr/local/bin/`.

## Goals

- Ship a signed, notarized, and stapled installer (`.pkg`).
- Keep a repeatable, scriptable workflow for CI or local release builds.
- Separate build, signing, packaging, and notarization steps for clarity and auditability.

## Preconditions

- Xcode Command Line Tools installed.
- Apple Developer ID certificates:
  - **Developer ID Application** (for signing binaries)
  - **Developer ID Installer** (for signing the `.pkg`)
- Keychain profile configured for `notarytool` (recommended):
  - `xcrun notarytool store-credentials --apple-id <id> --team-id <team> --password <app-specific-password> <profile_name>`

## Required Identifiers

You need stable identifiers for notarization and update tracking:

- **Bundle/Package identifier** (e.g. `com.agentd.pkg`)
- **Version** (e.g. `0.1.0` or `0.1.0+<git>`) — keep stable across artifacts

## Quick start (scripted)

Use the helper script (requires signing identities):

```bash
CODESIGN_IDENTITY="Developer ID Application: Your Name (TEAMID)" \
PKG_SIGN_IDENTITY="Developer ID Installer: Your Name (TEAMID)" \
NOTARY_PROFILE="your-notary-profile" \
tools/macos_package.sh
```

Artifacts are written under `out/macos_pkg_<timestamp>/` (including `SHA256SUMS.txt`).

## Manual steps (reference)

### 1) Build

```bash
cmake -S . -B build
cmake --build build -j
```

### 2) Stage binaries

```bash
mkdir -p stage/usr/local/bin
install -m 0755 build/agentd stage/usr/local/bin/agentd
install -m 0755 build/agent stage/usr/local/bin/agent
```

### 3) Codesign binaries (required for notarization)

```bash
codesign --force --options runtime --timestamp \
  --sign "Developer ID Application: Your Name (TEAMID)" \
  stage/usr/local/bin/agentd

codesign --force --options runtime --timestamp \
  --sign "Developer ID Application: Your Name (TEAMID)" \
  stage/usr/local/bin/agent
```

Verify:

```bash
codesign --verify --strict --deep stage/usr/local/bin/agentd
codesign --verify --strict --deep stage/usr/local/bin/agent
```

### 4) Build and sign the `.pkg`

```bash
pkgbuild \
  --root stage \
  --identifier com.agentd.pkg \
  --version 0.1.0 \
  --install-location / \
  unsigned.pkg

productsign \
  --sign "Developer ID Installer: Your Name (TEAMID)" \
  unsigned.pkg signed.pkg
```

### 5) Notarize and staple

```bash
xcrun notarytool submit signed.pkg --keychain-profile your-notary-profile --wait
xcrun stapler staple signed.pkg
```

Verify notarization:

```bash
xcrun stapler validate signed.pkg
```

## Operational notes

- `agentd` is not a GUI app; packaging as a `.pkg` is the most reliable distribution path.
- Use launchd for service management (see `tools/install_agentd_launchd.sh`).
- Keep the installer payload minimal: binaries + optional docs. Do not embed secrets.
- If you ship `agentd` to non-localhost targets, use `--auth-token` and CORS allowlists.

## Recommended environment variables

The helper script understands:

- `CODESIGN_IDENTITY` — Developer ID Application identity (required for notarization).
- `PKG_SIGN_IDENTITY` — Developer ID Installer identity (required to sign `.pkg`).
- `NOTARY_PROFILE` — notarytool keychain profile name (optional; enables notarization).
- `AGENTD_BIN`, `AGENT_BIN` — override binary paths.
- `MACOS_PKG_ID` — package identifier (default `com.agentd.pkg`).
- `MACOS_PKG_VERSION` — package version (default based on git + date).
- `MACOS_PKG_NAME` — output base name (default `agentd`).
- `MACOS_PKG_OUT_DIR` — output directory (default `out/macos_pkg_<timestamp>`).
- `MACOS_PKG_INSTALL_PREFIX` — install prefix (default `/usr/local/bin`).

## Security and auditability

- Never store credentials in the repo.
- Keep `NOTARY_PROFILE` in the login keychain or CI secrets store.
- Record the package checksum (SHA256) at release time.
- Retain signing logs for traceability.
