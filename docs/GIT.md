# Git Remote and Publishing

This guide covers setting up a git remote and publishing changes.

## Configure a remote

If `git push` fails with “No configured push destination”, set `origin` explicitly:

```bash
git remote add origin <your_repo_url>
git push -u origin "$(git rev-parse --abbrev-ref HEAD)"
```

If `origin` exists but points to the wrong place, pass `--force`:

```bash
tools/setup_git_remote.sh --url "<your_repo_url>" --force --push
```

## Helper script

The helper does not guess a URL:

```bash
AGENT_GIT_REMOTE_URL="<your_repo_url>" tools/setup_git_remote.sh --push
```

It can also read `git_remote` from your gitignored `project.local.md`:

```bash
cp project.local.md.example project.local.md
# edit project.local.md and set:
# - git_remote: <your_repo_url>
tools/setup_git_remote.sh --push
```

## Publish helper

To run a full local verify + push in one command:

```bash
tools/publish.sh --skip-ui
```
