#!/bin/sh
# Points sekraj.duckdns.org at the GitHub Pages site (sekyuraj.github.io).
# Before running: sign in at https://www.duckdns.org, add the domain "sekraj",
# and copy the token shown at the top of the page.
set -e
cd "$(dirname "$0")"
NAME=sekraj
DOMAIN=$NAME.duckdns.org
REPO=sekyuraj/sekyuraj.github.io
TOKFILE="$HOME/.config/duckdns/token"

if [ ! -s "$TOKFILE" ]; then
    printf "DuckDNS token: "
    stty -echo; read -r TOKEN; stty echo; echo
    mkdir -p "$(dirname "$TOKFILE")"
    umask 077; printf '%s\n' "$TOKEN" > "$TOKFILE"
fi
TOKEN=$(cat "$TOKFILE")

# DuckDNS holds one IPv4 per name; this is one of GitHub Pages' addresses.
r=$(curl -s "https://www.duckdns.org/update?domains=$NAME&token=$TOKEN&ip=185.199.108.153")
if [ "$r" != "OK" ]; then
    echo "duckdns said: $r  (wrong token, or you haven't added '$NAME' on duckdns.org yet)"
    rm -f "$TOKFILE"
    exit 1
fi
echo "dns: $DOMAIN -> GitHub Pages"

# Site repo has to exist first.
gh repo view "$REPO" >/dev/null 2>&1 || ./publish.sh

echo "$DOMAIN" > CNAME
./publish.sh
gh api -X PUT "repos/$REPO/pages" -f cname="$DOMAIN" >/dev/null
echo "custom domain set on GitHub"

echo "waiting for GitHub's HTTPS certificate (often 5-15 min)..."
i=0
while [ $i -lt 40 ]; do
    if gh api -X PUT "repos/$REPO/pages" -f cname="$DOMAIN" -F https_enforced=true >/dev/null 2>&1; then
        echo "done: https://$DOMAIN"
        exit 0
    fi
    i=$((i+1)); sleep 30
done
echo "the site already works at http://$DOMAIN; the cert is still pending."
echo "run this script again later to switch on HTTPS."
