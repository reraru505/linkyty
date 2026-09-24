# Kyty Linux Fork - Upstream Sync Utilities

These scripts allow you to pull graphics stack, shader compiler, and renderer improvements from the upstream KytyPS5 repository without needing an active `origin` remote or disturbing the Linux-only kernel changes.

---

## 1. `scripts/sync_graphics.sh`

### Inspecting Upstream
```bash
# Check status and see if any new graphics commits are available upstream
./scripts/sync_graphics.sh status

# Fetch the latest commits from upstream into local ref (refs/upstream/main)
./scripts/sync_graphics.sh fetch

# View the last 15 upstream commits that touched src/graphics/
./scripts/sync_graphics.sh log 15

# View the git diff of src/graphics/ between local HEAD and upstream
./scripts/sync_graphics.sh diff
```

### Syncing Changes
```bash
# Option A: Import entire upstream src/graphics/ tree (leaves kernel/CMake untouched)
./scripts/sync_graphics.sh import-tree

# Option B: Cherry-pick a specific upstream commit (ONLY applies changes to src/graphics/)
./scripts/sync_graphics.sh pick <commit-hash>
```

---

## 2. Remote Configuration

The repository currently has **no active remotes** (`git remote -v` is empty). All git operations stay completely local on this machine.

The `sync_graphics.sh` script talks directly over HTTPS to `https://github.com/KytyPS5/KytyPS5.git` on demand when you explicitly run `fetch` or `pick`.
