#!/bin/sh
# Create a connected PTY pair for manual PPPoS testing.
set -eu

command -v socat >/dev/null 2>&1 || {
  echo "socat is required (install socat first)" >&2
  exit 1
}

prefix=${1:-/tmp/mg-pppos}
mongoose_tty="${prefix}-mongoose"
pppd_tty="${prefix}-pppd"

echo "Creating connected PTYs:"
echo "  Mongoose: $mongoose_tty"
echo "  pppd:     $pppd_tty"
echo "Stop with Ctrl-C."
exec socat -d -d \
  "pty,raw,echo=0,link=$mongoose_tty" \
  "pty,raw,echo=0,link=$pppd_tty"
