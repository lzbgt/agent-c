# Publishing Agent-C snapshots

Owner direction, 2026-11-10: publish committed achievements to the private [bruce/agent-c repository](https://git.hubstack.cn/bruce/agent-c), **not GitHub**. The owner explicitly authorized this project destination after its historical removal. That authorization applies to Agent-C only; other excluded Panda repositories remain excluded.

The local `panda` remote is `https://git.hubstack.cn/bruce/agent-c.git`. The repository is private, with default branch `master`. GitHub `origin` remains a fetch source for existing upstream history. `remote.pushDefault=panda` and `branch.master.pushRemote=panda` select the private destination for ordinary pushes, but do not prohibit an explicit command targeting another remote. Publishing automation must use the policy helper below and must not push `origin`.

On an enrolled host, read its `PANDA_PUSH.md` and use the existing private automation credential through the host helper. Do not commit credentials or embed them in URLs, command arguments or logs.

```sh
python3 ~/.local/share/panda/push-project.py .
python3 ~/.local/share/panda/push-project.py . --push
```

The first command verifies access and current server policy without publishing. The second exports committed state with the owner's history/size policy, then verifies remote refs. It preserves uncommitted work and source history. The helper does not create a destination; an absent repository is not permission to recreate it.

Current server defaults are a 20-commit threshold and 20,000,000-byte stored repository limit. At or below the threshold, the helper exports committed branches/tags. Above it, the helper exports a depth-one shadow snapshot using isolated storage; source history is retained locally. Read live settings before publishing rather than assuming these defaults are permanent. Do not force push, raise limits or switch to a full-history push to bypass policy.

A fresh clone does not reproduce local remote preferences automatically. Set `panda` and the push preferences explicitly for the authorized existing destination, then verify them:

```sh
git remote add panda https://git.hubstack.cn/bruce/agent-c.git
git config remote.pushDefault panda
git config branch.master.pushRemote panda
git remote -v
python3 ~/.local/share/panda/push-project.py .
```

If the clone already names the private destination `origin`, retain that name and configure the push default accordingly; the helper discovers the existing HTTPS Panda remote. Check the current remote list before adding anything. Older GitHub publishing examples in historical operational notes do not supersede this owner direction.
