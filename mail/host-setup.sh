#!/bin/bash
# Turns on website hosting (/host/ + /~name/). Run as tux; asks for sudo.
set -e
cd "$(dirname "$0")"
[ "$(id -u)" = 0 ] && { echo "run as tux, not root"; exit 1; }
sudo -v
echo "== folder for hosted sites"
sudo install -d -o tux -g tux -m755 /srv/sekhost
echo "== sekmail (now with /host/)"
make -s
sudo install -m755 sekmaild /usr/local/bin/sekmaild
sudo install -o root -g root -m644 "$HOME/src/netsec/jail/sekmail" /etc/tux-jail/sekmail
sudo rc-service sekmail restart
echo "== Caddy"
sudo cp /etc/caddy/Caddyfile /etc/caddy/Caddyfile.before-host
sudo install -m644 ../selfhost/Caddyfile /etc/caddy/Caddyfile
if ! sudo rc-service caddy reload; then
    echo "Caddy refused the new config, putting the old one back"
    sudo cp /etc/caddy/Caddyfile.before-host /etc/caddy/Caddyfile
    sudo rc-service caddy reload
    exit 1
fi
../publish.sh
sleep 1
echo
for u in /host/ /mail/; do
    curl -s -m 10 -o /dev/null -w "https://sekraj.duckdns.org$u -> %{http_code}\n" "https://sekraj.duckdns.org$u"
done
echo "Requests show up at https://sekraj.duckdns.org/host/admin (log in as your admin account)."
