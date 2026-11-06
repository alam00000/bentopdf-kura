#!/bin/sh
set -e
case "$1" in
  ""|serve|coordinator|worker) exec /usr/local/bin/kura-server "$@" ;;
esac
exec /usr/local/bin/kura "$@"
