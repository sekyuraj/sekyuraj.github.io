#!/bin/sh
# Installs selfhost/Caddyfile as /etc/caddy/Caddyfile after checking it, then reloads Caddy.
set -e
cd "$(dirname "$0")"
caddy validate --config Caddyfile --adapter caddyfile >/dev/null 2>&1 || { echo "Caddyfile has an error:"; caddy validate --config Caddyfile --adapter caddyfile; exit 1; }
sudo install -m644 Caddyfile /etc/caddy/Caddyfile
sudo rc-service caddy restart
sleep 2
curl -sI https://sekraj.duckdns.org/ | grep -i '^content-security-policy' >/dev/null && echo "caddy: new headers live" || echo "caddy: restarted, but the CSP header isn't showing yet"
