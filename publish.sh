#!/bin/sh
# Puts ~/src/site online at https://sekyuraj.github.io (free, HTTPS, no port forwarding).
# Run it again any time after editing files to update the live site.
set -e
cd "$(dirname "$0")"
REPO=sekyuraj/sekyuraj.github.io

if [ ! -d .git ]; then
    git init -q -b main
fi
git add -A
git commit -qm "update site" 2>/dev/null || echo "nothing new to commit"

if ! gh repo view "$REPO" >/dev/null 2>&1; then
    gh repo create "$REPO" --public --source . --push
else
    git remote get-url origin >/dev/null 2>&1 || git remote add origin "https://github.com/$REPO.git"
    git push -u origin main
fi

echo
echo "live in ~1 minute at: https://sekyuraj.github.io"
