# Publishing Agent-C snapshots

The owner's latest direction is **publish this verified snapshot to GitHub**. This supersedes the earlier GitHub exclusion and local-only hold for this publication. **Panda publishing remains on hold** while its service is being fixed. Do not publish to Panda, recreate a missing/excluded destination or change its server policy during this hold.

## Current GitHub publication

The existing `origin` is `https://github.com/lzbgt/agent-c.git`, with branch `master`. Publish reviewed committed changes through the existing authenticated Git transport, verify that the update is a fast-forward, and compare the remote branch to the committed local HEAD afterward. Never include private configuration, credentials, generated artifacts or unrelated files. Do not force-push or rewrite existing remote history.

```sh
git push origin HEAD:refs/heads/master
git ls-remote origin refs/heads/master
```

Repository-local push preferences can select `origin` for this active direction. Preserve both remote definitions so a later explicit owner instruction can resume Panda publication without rebuilding source history.

Use the owner's existing GitHub credential helper. On this Arch host an existing
private `~/.git-credentials` store can be selected for the command with
`-c 'credential.helper=store --file /home/bruce/.git-credentials'`. Credentials stay
in that private store; do not copy them into the repository or log HTTP headers.

## Panda when publication is authorized again

The configured Panda remote is `https://git.hubstack.cn/bruce/agent-c.git`. Its presence is not proof that the destination currently exists. Read the enrolled host's `PANDA_PUSH.md`; Agent-C appears in its exclusion history, and a missing remote must never be recreated automatically.

Use the documented helper and its existing private automation credential. The helper does not create repositories. Never print credentials, commit them, embed them in URLs or pass them in command arguments.

```sh
python3 ~/.local/share/panda/push-project.py .
python3 ~/.local/share/panda/push-project.py . --push
```

These commands are retained for a future authorized Panda publication; do not run them during the present service hold.

The current host guide describes a 20-commit initialization threshold. Larger repositories start from a depth-one shadow of the committed HEAD, then retain subsequent history from that boundary. Existing shadows are not automatically deepened; refs outside the retained boundary remain omitted. Source history and working files stay intact. The documented 20,000,000-byte threshold is an administrator **warning**, not a stored-size prohibition. Separate upload, cache, memory and free-disk controls still apply. Read current settings before publication, and never force-update refs or bypass policy.

The owner authorized revising the history threshold when necessary; this does not require a server-wide change for ordinary local commits or the current GitHub publication.
