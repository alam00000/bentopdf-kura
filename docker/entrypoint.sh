#!/bin/sh
set -e
if [ "$#" -eq 0 ] || [ "$1" = "serve" ]; then
  exec node /kura/server/server.mjs
fi
exec /usr/local/bin/kura "$@"
