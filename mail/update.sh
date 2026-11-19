#!/bin/sh
# Rebuilds sekmaild and restarts it (after code changes). Asks for sudo.
set -e
cd "$(dirname "$0")"
make
sudo install -m755 sekmaild /usr/local/bin/sekmaild
sudo rc-service sekmail restart
