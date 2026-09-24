#!/usr/bin/env bash
# ==============================================================================
# sync_graphics.sh - Manage and pull upstream graphics/shader updates
# 
# Works in a standalone local repository without needing an active 'origin' remote.
# Fetches upstream changes directly from GitHub into a local ref (refs/upstream/main)
# and allows selective syncing or cherry-picking of src/graphics/.
# ==============================================================================

set -euo pipefail

UPSTREAM_URL="${KYTY_UPSTREAM_URL:-https://github.com/KytyPS5/KytyPS5.git}"
UPSTREAM_BRANCH="${KYTY_UPSTREAM_BRANCH:-main}"
UPSTREAM_REF="refs/upstream/${UPSTREAM_BRANCH}"

# Determine repository root
REPO_ROOT="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
cd "$REPO_ROOT"

print_help() {
    cat <<EOF
Kyty Linux Fork - Graphics Stack Synchronization Utility

Usage:
  $(basename "$0") <command> [arguments]

Commands:
  fetch
      Fetch the latest upstream commits from:
      $UPSTREAM_URL
      into local ref: $UPSTREAM_REF (no permanent remote needed).

  log [count]
      Show recent upstream commits affecting 'src/graphics/'.
      Default count is 15.

  diff
      Show a git diff for 'src/graphics/' between local HEAD and upstream.

  status
      Display local and upstream commit heads and ahead/behind statistics.

  import-tree [--dry-run]
      Overwrite local 'src/graphics/' with the exact tree from upstream.
      Leaves other directories (kernel, loader, common, CMake) untouched.

  pick <commit-hash>
      Cherry-pick a specific upstream commit, but ONLY apply changes that touch
      'src/graphics/'. Discards any conflicting platform/kernel changes.

  help
      Show this help message.

Environment Variables:
  KYTY_UPSTREAM_URL     Custom upstream URL (default: $UPSTREAM_URL)
  KYTY_UPSTREAM_BRANCH  Upstream branch name (default: $UPSTREAM_BRANCH)
EOF
}

cmd_fetch() {
    echo "==> Fetching from upstream: $UPSTREAM_URL ($UPSTREAM_BRANCH)..."
    git fetch "$UPSTREAM_URL" "$UPSTREAM_BRANCH:$UPSTREAM_REF" --tags --prune
    echo "==> Upstream fetched successfully into $UPSTREAM_REF"
    echo "    Latest upstream commit: $(git rev-parse --short "$UPSTREAM_REF") - $(git log -1 --pretty=%s "$UPSTREAM_REF")"
}

ensure_upstream_ref() {
    if ! git rev-parse --verify "$UPSTREAM_REF" >/dev/null 2>&1; then
        echo "==> Upstream ref $UPSTREAM_REF not found. Fetching now..."
        cmd_fetch
    fi
}

cmd_log() {
    ensure_upstream_ref
    local count="${1:-15}"
    echo "==> Recent upstream commits touching 'src/graphics/' (showing $count):"
    git log -n "$count" --oneline "$UPSTREAM_REF" -- src/graphics/
}

cmd_diff() {
    ensure_upstream_ref
    echo "==> Diff between HEAD and $UPSTREAM_REF for src/graphics/:"
    git diff --stat HEAD "$UPSTREAM_REF" -- src/graphics/
    echo ""
    echo "Tip: Run 'git diff HEAD $UPSTREAM_REF -- src/graphics/' for full patch output."
}

cmd_status() {
    ensure_upstream_ref
    local local_head
    local upstream_head
    local local_subject
    local upstream_subject

    local_head="$(git rev-parse --short HEAD)"
    local_subject="$(git log -1 --pretty=%s HEAD)"
    upstream_head="$(git rev-parse --short "$UPSTREAM_REF")"
    upstream_subject="$(git log -1 --pretty=%s "$UPSTREAM_REF")"

    echo "==> Synchronization Status:"
    echo "    Local HEAD:      $local_head - \"$local_subject\""
    echo "    Upstream Target: $upstream_head - \"$upstream_subject\""
    echo ""
    echo "    Pending graphics commits in upstream not in local branch:"
    git log --oneline HEAD.."$UPSTREAM_REF" -- src/graphics/ | head -n 10 || true
}

cmd_import_tree() {
    ensure_upstream_ref
    local dry_run=0
    if [[ "${1:-}" == "--dry-run" ]]; then
        dry_run=1
    fi

    echo "==> Checking out src/graphics/ from $UPSTREAM_REF..."
    if [[ $dry_run -eq 1 ]]; then
        echo "    [Dry-run] Would update the following files in src/graphics/:"
        git diff --name-status HEAD "$UPSTREAM_REF" -- src/graphics/
        return 0
    fi

    # Ensure working tree in src/graphics is clean
    if ! git diff --quiet HEAD -- src/graphics/; then
        echo "ERROR: You have uncommitted changes in 'src/graphics/'. Please commit or stash them first." >&2
        exit 1
    fi

    git checkout "$UPSTREAM_REF" -- src/graphics/
    echo "==> src/graphics/ has been updated and staged to match upstream $UPSTREAM_REF."
    echo "    Run 'git status' and 'git commit -m \"upstream: sync src/graphics with $UPSTREAM_REF\"' to commit."
}

cmd_pick() {
    local commit="${1:-}"
    if [[ -z "$commit" ]]; then
        echo "ERROR: Commit hash required. Usage: $(basename "$0") pick <commit-hash>" >&2
        exit 1
    fi

    ensure_upstream_ref

    # Verify commit exists in repository history
    if ! git rev-parse --verify "$commit^{commit}" >/dev/null 2>&1; then
        echo "==> Commit $commit not in local database. Attempting to fetch directly..."
        git fetch "$UPSTREAM_URL" "$commit" || {
            echo "ERROR: Failed to fetch commit $commit from $UPSTREAM_URL" >&2
            exit 1
        }
    fi

    local commit_msg
    commit_msg="$(git log -1 --pretty="%s (%an)" "$commit")"
    echo "==> Picking graphics changes from: $commit - \"$commit_msg\""

    # Check if commit actually touches src/graphics
    local graphics_files
    graphics_files="$(git diff-tree --no-commit-id --name-only -r "$commit" -- src/graphics/)"

    if [[ -z "$graphics_files" ]]; then
        echo "WARNING: Commit $commit does not touch any files in 'src/graphics/'."
        echo "Changed files in commit:"
        git diff-tree --no-commit-id --name-only -r "$commit"
        exit 0
    fi

    echo "    Files to apply:"
    echo "$graphics_files" | sed 's/^/      - /'

    # Apply only the graphics changes directly
    git checkout "$commit" -- src/graphics/
    echo "==> Applied changes from $commit to src/graphics/ (staged in index)."
    echo "    To commit with original message, run:"
    echo "      git commit -m \"upstream: cherry-pick $commit_msg\""
}

# Subcommand dispatcher
COMMAND="${1:-help}"
shift || true

case "$COMMAND" in
    fetch)
        cmd_fetch "$@"
        ;;
    log)
        cmd_log "$@"
        ;;
    diff)
        cmd_diff "$@"
        ;;
    status)
        cmd_status "$@"
        ;;
    import-tree)
        cmd_import_tree "$@"
        ;;
    pick|cherry-pick)
        cmd_pick "$@"
        ;;
    help|--help|-h)
        print_help
        ;;
    *)
        echo "Unknown command: '$COMMAND'" >&2
        echo ""
        print_help
        exit 1
        ;;
esac
