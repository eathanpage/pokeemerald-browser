#!/usr/bin/env bash
# Builds the browser port and publishes port/dist to the gh-pages branch,
# which GitHub Pages serves (Settings -> Pages -> Deploy from a branch ->
# gh-pages, / (root)).
#
# Usage, from the repository root: port/tools/deploy_pages.sh [remote]
set -euo pipefail

remote="${1:-origin}"
root="$(git rev-parse --show-toplevel)"
cd "$root"

make -f port/wasm.mk -j"$(nproc)"

source_commit="$(git rev-parse --short HEAD)"
worktree="$(mktemp -d)"
trap 'git worktree remove --force "$worktree" >/dev/null 2>&1 || rm -rf "$worktree"' EXIT

# Start from the current gh-pages branch if there is one, so the history
# only grows by one commit per deploy.
if git fetch -q "$remote" gh-pages 2>/dev/null; then
    git worktree add -q --detach "$worktree" FETCH_HEAD
else
    git worktree add -q --detach "$worktree"
    git -C "$worktree" checkout -q --orphan gh-pages-tmp
fi

git -C "$worktree" rm -rfq . >/dev/null 2>&1 || true
cp -r port/dist/. "$worktree"/
# No Jekyll processing; serve the files as they are.
touch "$worktree/.nojekyll"

git -C "$worktree" add -A
if git -C "$worktree" diff --cached --quiet; then
    echo "gh-pages is already up to date."
    exit 0
fi
git -C "$worktree" commit -q -m "Deploy browser port from $source_commit"
git -C "$worktree" push "$remote" HEAD:refs/heads/gh-pages
echo "Deployed $source_commit to gh-pages."
