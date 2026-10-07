#!/bin/bash
# Re-create every commit of a branch (on top of its base) with the current
# git identity as the author (git commit --reset-author) and without any
# "Co-Authored-By:" trailer.
#
# Usage: reauthor-branch.sh [base] [branch]
#   base    defaults to "main"
#   branch  defaults to the current branch
#
# The original branch is kept as "<branch>-orig" so nothing is lost; delete
# it once you are happy with the result. Run it from within the repository,
# with a clean working tree. Hooks are skipped (--no-verify), as the commits
# have already been checked.

set -euo pipefail

base=${1:-main}
branch=${2:-$(git symbolic-ref --short HEAD)}
backup="$branch-orig"

if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
    echo "error: the working tree has uncommitted changes." >&2
    exit 1
fi
if git show-ref --verify --quiet "refs/heads/$backup"; then
    echo "error: backup branch '$backup' already exists." >&2
    exit 1
fi
if [ -n "$(git rev-list --merges "$base..$branch")" ]; then
    echo "error: '$branch' contains merge commits; not supported." >&2
    exit 1
fi

commits=$(git rev-list --reverse "$base..$branch")
if [ -z "$commits" ]; then
    echo "Nothing to do: '$branch' has no commits on top of '$base'."
    exit 0
fi

echo "Author will be: $(git config user.name) <$(git config user.email)>"
echo "Rewriting $(echo "$commits" | wc -l | tr -d ' ') commit(s) of '$branch' on top of '$base'."

git branch "$backup" "$branch"
git switch --quiet --detach "$(git merge-base "$base" "$branch")"

msg=$(mktemp)
trap 'rm -f "$msg"' EXIT

for c in $commits; do
    git cherry-pick --allow-empty --keep-redundant-commits "$c" >/dev/null
    # Drop the trailer lines; git strips the trailing blank line left behind.
    git log -1 --format=%B "$c" | grep -v -i '^Co-Authored-By:' > "$msg"
    git commit --amend --quiet --no-verify --reset-author --cleanup=whitespace -F "$msg"
    echo "  $(git log -1 --format='%h %s')"
done

git branch --force "$branch" HEAD
git switch --quiet "$branch"

echo "Done. The original commits are kept in '$backup'."
echo "Check with:  git log --format='%h %an <%ae>%n%B' $base..$branch"
echo "             git diff $backup $branch    # must be empty"
