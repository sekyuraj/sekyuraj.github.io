#!/bin/sh
# Sets up sekcloud at https://sekraj.duckdns.org/cloud/ with its own
# Bitcoin node for checking payments. Run as tux; asks for sudo. Safe to rerun.
#
# Your coins live only in Electrum (your 12 words). sekcloud and the node
# get a watch-only key: they can see payments but can never spend them.
set -e
cd "$(dirname "$0")"
DATA=$HOME/.local/share/sekcloud
CONF=/etc/bitcoin/bitcoin.conf

[ "$(id -u)" = 0 ] && { echo "run as tux, not root"; exit 1; }
sudo -v

say() { printf '\n== %s\n' "$*"; }

# ---------------------------------------------------------------- packages
say "Bitcoin Core, Electrum, qrencode"
need=""
command -v bitcoind >/dev/null || need="$need net-p2p/bitcoin-core"
command -v electrum >/dev/null || need="$need net-misc/electrum"
command -v qrencode >/dev/null || need="$need media-gfx/qrencode"
if [ -n "$need" ]; then
    sudo tee /etc/portage/package.use/sekcloud >/dev/null <<'EOF'
# sekcloud: Bitcoin node without its GUI, plus what Electrum's Qt6 window needs
net-p2p/bitcoin-core cli daemon wallet -gui
>=dev-libs/libsecp256k1-0.7.1 musig
>=dev-python/pyqt6-6.11.0-r1 qml quick multimedia
>=dev-qt/qtdeclarative-6.11.2-r1 opengl
EOF
    sudo rm -f /etc/portage/package.use/bitcoin-core
    echo "compiling:$need (Bitcoin Core takes a while)"
    sudo emerge --ask=n --noreplace $need
fi

# ---------------------------------------------------------------- node
say "Bitcoin node"
mkdir -p "$DATA"
chmod 700 "$DATA"
RPCPASS=$(node -e 'try{process.stdout.write(require(process.argv[1]).rpcPassword||"")}catch{}' "$DATA/config.json")
if [ -z "$RPCPASS" ] || ! sudo grep -q '^rpcauth=sekcloud:' "$CONF" 2>/dev/null; then
    RPCPASS=$(node -e 'process.stdout.write(require("crypto").randomBytes(24).toString("base64url"))')
    SALT=$(node -e 'process.stdout.write(require("crypto").randomBytes(16).toString("hex"))')
    HASH=$(node -e 'const c=require("crypto");process.stdout.write(c.createHmac("sha256",process.argv[1]).update(process.argv[2]).digest("hex"))' "$SALT" "$RPCPASS")
    [ -f "$CONF" ] && sudo cp "$CONF" "$CONF.orig"
    sudo tee "$CONF" >/dev/null <<EOF
# Bitcoin node for sekcloud payments, written by ~/src/site/cloud/install.sh
server=1
# keep only the last ~10 GB of blocks; enough to see new payments
prune=10000
# memory for the first sync (MB); lower it after the sync if you like
dbcache=4000
# no incoming connections, so no port forward needed
listen=0
rpcbind=127.0.0.1
rpcallowip=127.0.0.1
rpcauth=sekcloud:$SALT\$$HASH
EOF
    sudo chown bitcoin:bitcoin "$CONF"
    sudo chmod 660 "$CONF"
    node -e '
        const fs = require("fs"), f = process.argv[1];
        let c = {}; try { c = JSON.parse(fs.readFileSync(f)); } catch {}
        c.rpcUser = "sekcloud"; c.rpcPassword = process.argv[2];
        fs.writeFileSync(f, JSON.stringify(c, null, 2) + "\n", { mode: 0o600 });' "$DATA/config.json" "$RPCPASS"
    RESTART=1
fi
sudo rc-update add bitcoind default >/dev/null
# Gentoo's bitcoind service reports "inactive" (non-zero) while it starts;
# the RPC wait below is the real check
if [ -n "$RESTART" ]; then sudo rc-service bitcoind restart || true; else sudo rc-service bitcoind start || true; fi

cli() { printf '%s\n' "$RPCPASS" | bitcoin-cli -rpcuser=sekcloud -stdinrpcpass "$@"; }
printf 'waiting for the node'
i=0
until cli getblockchaininfo >/dev/null 2>&1; do
    i=$((i+1))
    [ $i -gt 90 ] && { echo; echo "bitcoind isn't answering; see /var/log/bitcoind/debug.log"; exit 1; }
    printf .; sleep 2
done
echo " up"

# ---------------------------------------------------------------- wallet
if ! grep -q '"descriptor": "wpkh' "$DATA/config.json" 2>/dev/null; then
    say "your wallet"
    cat <<'EOF'
This part is yours alone. Nobody, me included, should ever see your 12 words.

  1. Open Electrum (run: electrum), click through to "Create new wallet".
  2. Pick "Standard wallet", then "Create a new seed".
  3. Write the 12 words on paper, twice, and keep them somewhere offline.
     Anyone with these words has your money. They're also the only way
     back in if this PC dies.
  4. Set a strong password for the wallet file.
  5. In Electrum: Wallet -> Information. Copy the Master Public Key
     (it starts with zpub). That key can only watch, not spend.
EOF
    printf '\nPaste the zpub here: '
    read -r ZPUB
    node server.js setup-wallet "$ZPUB"
    echo "Check that address against Electrum: View -> Show Addresses, the first receiving one."
    printf 'Press Enter if they match (Ctrl+C if not)... '
    read -r _
fi

# ---------------------------------------------------------------- service
say "sekcloud service"
sudo install -m755 sekcloud.initd /etc/init.d/sekcloud
sudo rc-update add sekcloud default >/dev/null
sudo rc-service sekcloud restart

say "Servers panel"
"$HOME/src/tux-servers/install-sudoers.sh"
make -C "$HOME/src/tux-servers" install
pkill -x web-control-ce 2>/dev/null || true   # the panel starts the new one next time it opens

say "website"
sudo install -Dm644 ../selfhost/Caddyfile /etc/caddy/Caddyfile
sudo rc-service caddy reload
../publish.sh

sleep 1
curl -s -m 10 -o /dev/null -w '\nhttps://sekraj.duckdns.org/cloud/  -> %{http_code}\n' https://sekraj.duckdns.org/cloud/ || true
cli getblockchaininfo | node -e 'let s="";process.stdin.on("data",d=>s+=d).on("end",()=>{const j=JSON.parse(s);console.log(`node sync: ${(j.verificationprogress*100).toFixed(1)}%${j.initialblockdownload?" (signups open when it reaches the end)":", signups are open"}`)})'
