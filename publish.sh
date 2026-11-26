#!/bin/sh
# Puts ~/src/site live on https://sekraj.duckdns.org (served from this PC).
# Run it again any time after editing files.
#
# Only committed files go online, minus the scripts and tools, so things
# like shots-raw/ (gitignored) never get served.
# The GitHub repo still gets a push as a backup copy (sekyuraj.github.io).
set -e
cd "$(dirname "$0")"

# ./publish.sh "what changed"  (the commit message; asked for when left out)
msg=$1
if [ -z "$msg" ] && [ -n "$(git status --porcelain)" ]; then printf 'what changed? '; read -r msg; fi
git add -A
git commit -qm "${msg:-site update}" 2>/dev/null || echo "nothing new to commit"

git ls-files \
  | grep -vE '^(CNAME|\.gitignore|\.nojekyll|[^/]*\.sh|tools/|selfhost/|server/|chan/|cloud/|mail/)' \
  | rsync -a --files-from=- ./ /srv/sekraj/
# --files-from doesn't delete; clear out files that are gone from git
( cd /srv/sekraj && find . -type f | sed 's|^\./||' ) | while read -r f; do
    git ls-files --error-unmatch -- "$f" >/dev/null 2>&1 || rm -f "/srv/sekraj/$f"
done
find /srv/sekraj -mindepth 1 -type d -empty -delete
echo "live: https://sekraj.duckdns.org"

# PUBLISH_API=1 ./publish.sh ...  -> also restart the Node backend (after editing server/)
if [ -n "$PUBLISH_API" ]; then
    sudo rc-service sekraj-api restart
fi

git push -q origin main 2>/dev/null && echo "backup pushed to GitHub" || true
