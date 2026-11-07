#!/bin/sh
set -eu

app=${1:?usage: run_pty.sh APP SOCAT PPPD}
socat=${2:?usage: run_pty.sh APP SOCAT PPPD}
pppd=${3:?usage: run_pty.sh APP SOCAT PPPD}

if [ "$(id -u)" -ne 0 ]; then
  echo "Run this script as root: pppd requires privilege for the noauth test peer." >&2
  exit 1
fi

tmpdir=$(mktemp -d)
socat_pid=
pppd_pid=
app_pid=
cleanup() {
  [ -z "$app_pid" ] || kill "$app_pid" 2>/dev/null || true
  [ -z "$pppd_pid" ] || kill "$pppd_pid" 2>/dev/null || true
  [ -z "$socat_pid" ] || kill "$socat_pid" 2>/dev/null || true
  [ -z "$app_pid" ] || wait "$app_pid" 2>/dev/null || true
  [ -z "$pppd_pid" ] || wait "$pppd_pid" 2>/dev/null || true
  [ -z "$socat_pid" ] || wait "$socat_pid" 2>/dev/null || true
  if [ -f "$tmpdir/pppd.log" ]; then cat "$tmpdir/pppd.log"; fi
  rm -rf "$tmpdir"
}
trap cleanup EXIT INT TERM

"$socat" "pty,raw,echo=0,link=$tmpdir/mongoose" \
  "pty,raw,echo=0,link=$tmpdir/pppd" >"$tmpdir/socat.log" 2>&1 &
socat_pid=$!
i=0
while [ ! -e "$tmpdir/mongoose" ] || [ ! -e "$tmpdir/pppd" ]; do
  i=$((i + 1))
  if [ "$i" -ge 50 ]; then
    cat "$tmpdir/socat.log" >&2
    exit 1
  fi
  sleep 0.1
done

"$pppd" "$tmpdir/pppd" 115200 local noauth nodetach nocrtscts \
  noaccomp nopcomp noipv6 debug logfd 2 192.0.2.1:192.0.2.2 \
  >"$tmpdir/pppd.log" 2>&1 &
pppd_pid=$!
"$app" "$tmpdir/mongoose" >"$tmpdir/app.log" 2>&1 &
app_pid=$!

i=0
while [ "$i" -lt 100 ]; do
  if command -v curl >/dev/null 2>&1 &&
     curl --silent --fail http://127.0.0.1:8080/ >/dev/null; then
    echo "PASS: HTTP reached Mongoose over the PPPoS PTY link"
    cat "$tmpdir/app.log"
    exit 0
  fi
  if ! kill -0 "$pppd_pid" 2>/dev/null; then
    echo "pppd exited before the PPP link came up" >&2
    cat "$tmpdir/pppd.log" >&2
    cat "$tmpdir/app.log" >&2
    exit 1
  fi
  i=$((i + 1))
  sleep 0.1
done

echo "Timed out waiting for HTTP over PPPoS" >&2
cat "$tmpdir/pppd.log" >&2
cat "$tmpdir/app.log" >&2
exit 1
