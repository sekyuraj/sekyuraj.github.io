#!/bin/bash
# Puts sekmail live: webmail at https://sekraj.duckdns.org/mail/ and
# incoming mail on port 25. Run as tux; asks for the sudo password. Safe to rerun.
set -e
cd "$(dirname "$0")"
DATA=$HOME/.local/share/sekmail

[ "$(id -u)" = 0 ] && { echo "run as tux, not root"; exit 1; }
sudo -v
say() { printf '\n== %s\n' "$*"; }

# libcurl needs SMTP for the optional relay (mail to outside addresses)
if ! curl -V | grep -q ' smtp'; then
    say "curl with SMTP"
    echo "net-misc/curl smtp" | sudo tee /etc/portage/package.use/sekmail >/dev/null
    sudo emerge --ask=n --oneshot net-misc/curl
fi

say "build"
make
sudo install -m755 sekmaild /usr/local/bin/sekmaild

mkdir -p "$DATA"
chmod 700 "$DATA"
if [ ! -f "$DATA/config" ]; then
    cat > "$DATA/config" <<'CONF'
# sekmail settings. Restart sekmail after editing.
domain = sekraj.duckdns.org
smtp_port = 25
http_port = 3003
max_size_mb = 20
quota_mb = 500
signups = open
signups_per_ip = 3
sends_per_day = 100
# your account: gets mail sent to postmaster@, abuse@, admin@ ...
admin =
# Mail to other servers. Outgoing port 25 is blocked on this line, so it
# has to go through an SMTP account you own (587 works), for example:
#   relay_url  = smtp://smtp.gmail.com:587
#   relay_user = you@gmail.com
#   relay_pass = an app password
#   relay_from = you@gmail.com   (envelope sender the relay accepts)
relay_url =
relay_user =
relay_pass =
relay_from =
CONF
    chmod 600 "$DATA/config"
fi

if ! grep -q '^admin *= *[a-z0-9]' "$DATA/config"; then
    say "your account"
    printf 'name for your own address (name@sekraj.duckdns.org): '
    read -r NAME
    printf 'password (8+ characters): '
    stty -echo; read -r PASS; stty echo; echo
    printf '%s\n' "$PASS" | ./sekmaild adduser "$NAME"
    sed -i "s/^admin *=.*/admin = $NAME/" "$DATA/config"
fi

say "service"
sudo install -m755 sekmail.initd /etc/init.d/sekmail
sudo rc-update add sekmail default >/dev/null
sudo rc-service sekmail restart

say "Web Control Center"
"$HOME/src/tux-servers/install-sudoers.sh"
make -C "$HOME/src/tux-servers" install
pkill -x web-control-ce 2>/dev/null || true

say "website"
sudo install -Dm644 ../selfhost/Caddyfile /etc/caddy/Caddyfile
sudo rc-service caddy reload
../publish.sh

sleep 1
echo
curl -s -m 10 -o /dev/null -w 'https://sekraj.duckdns.org/mail/  -> %{http_code}\n' https://sekraj.duckdns.org/mail/ || true
printf 'local SMTP: '; (exec 3<>/dev/tcp/127.0.0.1/25 && head -n1 <&3) 2>/dev/null || echo "not answering"
cat <<'MSG'

Last step, in the Speedport (192.168.2.1): forward TCP port 25 to this PC
(192.168.2.107), same as 80/443. Without it you can still mail other
@sekraj.duckdns.org users, but nothing from Gmail etc. gets in.
MSG
