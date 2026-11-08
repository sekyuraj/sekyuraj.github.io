#!/bin/sh
# Caddy crashed on start: `caddy validate` runs as root and created
# /var/log/caddy/access.log as root (0600), so Caddy (user http) couldn't open it.
# Also its crash messages went nowhere. This fixes both and finishes selfhost.sh.
set -e
cd "$(dirname "$0")"
sudo -v
sudo rc-service caddy stop >/dev/null 2>&1 || true
sudo install -Dm644 Caddyfile /etc/caddy/Caddyfile
sudo rm -f /var/log/caddy/access.log
sudo chown http:http /var/log/caddy/caddy.log 2>/dev/null || true
grep -q '^output_log' /etc/conf.d/caddy || \
    printf 'output_log="/var/log/caddy/caddy.log"\nerror_log="/var/log/caddy/caddy.log"\n' | sudo tee -a /etc/conf.d/caddy >/dev/null
sudo rc-service caddy start

echo "waiting for the HTTPS cert..."
i=0
while [ $i -lt 30 ]; do
    if [ ! -e /run/openrc/started/caddy ]; then
        echo "caddy stopped again, last log lines:"; tail -n 15 /var/log/caddy/caddy.log; exit 1
    fi
    if grep -q 'certificate obtained successfully' /var/log/caddy/caddy.log; then echo "cert: ok"; break; fi
    if grep -qE '"level":"error".*(acme|challenge|certificate)' /var/log/caddy/caddy.log; then
        echo "cert failed, last error:"; grep '"level":"error"' /var/log/caddy/caddy.log | tail -n 1 | cut -c1-400
        echo "(usually the port forward for 80/443; Caddy keeps retrying by itself)"
        break
    fi
    i=$((i+1)); sleep 4
done

grep -q 'sekraj.duckdns.org' /etc/hosts || \
    echo '127.0.0.1 sekraj.duckdns.org   # self-hosted site' | sudo tee -a /etc/hosts >/dev/null
sudo rc-service sekraj-api status >/dev/null 2>&1 || sudo rc-service sekraj-api start
curl -s -m 10 -o /dev/null -w 'https://sekraj.duckdns.org/  -> %{http_code}\n' https://sekraj.duckdns.org/ || echo "site test failed"
curl -s -m 10 https://sekraj.duckdns.org/api/hello; echo
