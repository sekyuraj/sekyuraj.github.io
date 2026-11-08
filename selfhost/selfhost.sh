#!/bin/sh
# Moves sekraj.duckdns.org from GitHub Pages to this PC.
#
#   Caddy      ports 80/443, free Let's Encrypt cert, serves /srv/sekraj
#   sekraj-api Node backend (~/src/site/server/server.js), gets /api/*
#   duckdns    keeps the name pointed at the house's public IP
#
# Run as tux (not with sudo); it asks for the sudo password itself.
set -e
cd "$(dirname "$0")"
SITE=$(cd .. && pwd)
LANIP=$(ip -4 -br addr show enp10s0 | awk '{print $3}' | cut -d/ -f1)

say() { printf '\n== %s\n' "$*"; }

[ "$(id -u)" = 0 ] && { echo "run as tux, not root"; exit 1; }
[ -s "$HOME/.config/duckdns/token" ] || { echo "no DuckDNS token in ~/.config/duckdns/token"; exit 1; }
sudo -v

say "installing caddy"
if ! command -v caddy >/dev/null; then
    sudo emerge --ask=n --noreplace www-servers/caddy
fi

say "installing config + services"
[ -f /etc/caddy/Caddyfile ] && ! cmp -s Caddyfile /etc/caddy/Caddyfile && \
    sudo cp /etc/caddy/Caddyfile /etc/caddy/Caddyfile.orig
sudo install -Dm644 Caddyfile /etc/caddy/Caddyfile
sudo install -d -o http -g http -m 750 /var/lib/caddy
sudo install -d -o http -g http -m 755 /var/log/caddy
sudo install -d -o tux -g tux -m 755 /srv/sekraj
sudo install -m755 sekraj-api.initd /etc/init.d/sekraj-api
sudo install -m755 duckdns.initd /etc/init.d/duckdns
sudo install -m755 duckdns-update /usr/local/sbin/duckdns-update
sudo caddy validate --config /etc/caddy/Caddyfile >/dev/null

say "copying the site to /srv/sekraj"
"$SITE/publish.sh"

say "starting the backend"
sudo rc-update add sekraj-api default >/dev/null
sudo rc-service sekraj-api restart
sleep 1
curl -sf http://127.0.0.1:3000/api/hello && echo

cat <<EOF

== router (do this now, in the browser)
Open http://speedport.ip and log in with the device password
(it's on the sticker on the back of the Speedport).

  1. Heimnetz -> Heimnetzwerk (LAN) -> Übersicht -> this PC ($LANIP)
     tick "Immer die gleiche IPv4-Adresse zuweisen", save.
  2. Internet -> Portfreischaltung -> Port-Umleitungen und Port-Weiterleitungen
     add one for this PC ($LANIP):
        TCP 80  -> 80
        TCP 443 -> 443
        TCP 25565 -> 25565   (Minecraft server)
     save.

EOF
printf 'Press Enter when both are saved... '
read -r _

say "taking the domain back from GitHub"
cd "$SITE"
if [ -f CNAME ]; then
    git rm -q CNAME
    git commit -qm "site moved to self-hosting; github.io stays as a mirror"
    git push -q origin main || echo "(push failed, do it later)"
fi
echo '{"cname":null}' | gh api -X PUT repos/sekyuraj/sekyuraj.github.io/pages --input - >/dev/null 2>&1 \
    && echo "GitHub custom domain removed (sekyuraj.github.io still works)" \
    || echo "(couldn't clear GitHub's custom domain; harmless, do it in repo Settings -> Pages)"
cd - >/dev/null

say "pointing sekraj.duckdns.org at this house"
sudo rc-update add duckdns default >/dev/null
sudo rc-service duckdns restart
sleep 3
tail -n1 /var/log/duckdns.log

say "starting caddy (gets the HTTPS cert)"
sudo rc-update add caddy default >/dev/null
sudo rc-service caddy restart
# DuckDNS TTL is 60 s, give Let's Encrypt a moment to see the new address
i=0
while [ $i -lt 36 ]; do
    if sudo grep -q 'certificate obtained successfully' /var/log/caddy/caddy.log 2>/dev/null; then
        echo "cert: ok"; break
    fi
    if sudo grep -qE 'could not get certificate|Timeout during connect|Connection refused' /var/log/caddy/caddy.log 2>/dev/null; then
        echo "cert: Let's Encrypt can't reach this PC yet (port forward?). Caddy keeps retrying by itself."
        break
    fi
    i=$((i+1)); sleep 5
done

# The Speedport may not loop its own public IP back inside the house,
# so this PC talks to itself directly.
say "local shortcut in /etc/hosts"
grep -q 'sekraj.duckdns.org' /etc/hosts || \
    echo '127.0.0.1 sekraj.duckdns.org   # self-hosted site, see ~/src/site/selfhost' | sudo tee -a /etc/hosts >/dev/null
echo "ok"

say "test"
curl -s -m 10 -o /dev/null -w 'https://sekraj.duckdns.org/  -> %{http_code}\n' https://sekraj.duckdns.org/ || true
curl -s -m 10 https://sekraj.duckdns.org/api/hello || true
echo
echo
echo "done. logs: /var/log/caddy/caddy.log  /var/log/sekraj-api.log  /var/log/duckdns.log"
echo "edit pages, then ./publish.sh   (edited server/? ./publish.sh api)"
