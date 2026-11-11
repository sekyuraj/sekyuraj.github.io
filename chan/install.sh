#!/bin/sh
# Puts tuxchan live at https://sekraj.duckdns.org/chan/ and adds it to
# the Servers panel. Run as tux; it asks for the sudo password. Safe to rerun.
set -e
cd "$(dirname "$0")"

[ "$(id -u)" = 0 ] && { echo "run as tux, not root"; exit 1; }
command -v ffmpeg >/dev/null || { echo "tuxchan needs ffmpeg (media-video/ffmpeg)"; exit 1; }
sudo -v

if ! grep -q '"modHash": "[^"]' "$HOME/.local/share/tuxchan/config.json" 2>/dev/null; then
    echo "Pick a moderator password. You log in with it at /chan/mod"
    stty -echo
    node server.js passwd || { stty echo; exit 1; }
    stty echo
    echo
fi

echo "== service"
sudo install -m755 tuxchan.initd /etc/init.d/tuxchan
sudo rc-update add tuxchan default >/dev/null
sudo rc-service tuxchan restart

echo "== Servers panel"
"$HOME/src/tux-servers/install-sudoers.sh"
make -C "$HOME/src/tux-servers" install
pkill -x web-control-ce 2>/dev/null || true   # the panel starts the new one next time it opens

echo "== website"
sudo install -Dm644 ../selfhost/Caddyfile /etc/caddy/Caddyfile
sudo rc-service caddy reload
../publish.sh

sleep 1
echo
curl -s -m 10 -o /dev/null -w 'https://sekraj.duckdns.org/chan/  -> %{http_code}\n' https://sekraj.duckdns.org/chan/ || true
echo "Moderate at https://sekraj.duckdns.org/chan/mod"
echo "Boards, rules and limits: ~/.local/share/tuxchan/config.json (restart tuxchan after editing)"
