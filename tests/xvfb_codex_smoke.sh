#!/bin/sh

set -eu

: "${PRAGTICAL_BIN:?set PRAGTICAL_BIN to the rebuilt Pragtical executable}"

if [ "${CODEX_BIN:-}" != "" ]; then
  codex_bin=$CODEX_BIN
else
  codex_bin=$(command -v codex)
fi
: "${codex_bin:?set CODEX_BIN to the Codex executable}"

test_dir=$(mktemp -d /tmp/pragtical-terminal-codex-xvfb.XXXXXX)
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
LIBTSM_LIBRARY_PATH=${LIBTSM_LIBRARY_PATH:-}
export LIBTSM_LIBRARY_PATH

cleanup() {
  status=$?
  trap - EXIT
  rm -rf "$test_dir"
  exit "$status"
}
trap cleanup EXIT HUP INT TERM

xvfb-run -a -s '-screen 0 1280x800x24 -nolisten tcp' sh -c '
  set -eu
  test_dir=$1
  proxy=$2
  app_bin=$3
  codex_bin=$4

  if [ "$LIBTSM_LIBRARY_PATH" != "" ]; then
    LD_LIBRARY_PATH="$LIBTSM_LIBRARY_PATH${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    export LD_LIBRARY_PATH
  fi
  PRAGTICAL_USERDIR="$test_dir/user"
  CODEX_HOME="$test_dir/codex-home"
  export PRAGTICAL_USERDIR CODEX_HOME
  mkdir -p "$CODEX_HOME"
  SDL_VIDEO_DRIVER=x11
  SDL_VIDEODRIVER=x11
  SDL_RENDER_DRIVER=software
  LIBGL_ALWAYS_SOFTWARE=1
  export SDL_VIDEO_DRIVER SDL_VIDEODRIVER SDL_RENDER_DRIVER LIBGL_ALWAYS_SOFTWARE

  "$app_bin" >"$test_dir/pragtical.log" 2>&1 &
  app_pid=$!
  cleanup_app() {
    status=$?
    trap - EXIT
    kill "$app_pid" 2>/dev/null || true
    wait "$app_pid" 2>/dev/null || true
    exit "$status"
  }
  trap cleanup_app EXIT HUP INT TERM

  window=
  previous_window=
  for _ in $(seq 1 100); do
    candidate=$(xdotool search --class "dev.pragtical.Pragtical" 2>/dev/null \
      | head -n 1 || true)
    if [ "$candidate" != "" ] && [ "$candidate" = "$previous_window" ]; then
      window=$candidate
      break
    fi
    previous_window=$candidate
    kill -0 "$app_pid" 2>/dev/null
    sleep 0.1
  done
  if [ "$window" = "" ]; then
    echo "xvfb Codex smoke: Pragtical window was not created" >&2
    exit 1
  fi

  # Pragtical creates the SDL window hidden and normally relies on the desktop
  # window manager to map it after the first frame. Map it explicitly here;
  # this keeps the smoke test independent of a particular desktop WM.
  xdotool windowmap "$window" 2>/dev/null || true
  xdotool windowraise "$window" 2>/dev/null || true
  xdotool windowfocus "$window" 2>/dev/null || true
  sleep 0.3
  xdotool key --clearmodifiers --window "$window" shift+alt+t
  sleep 0.3
  xdotool mousemove --window "$window" 600 700
  xdotool click 1

  status_path="$test_dir/codex-status.txt"
  command="python3 '\''$proxy'\'' '\''$codex_bin'\'' '\''$status_path'\''"
  xdotool type --clearmodifiers --window "$window" --delay 1 "$command"
  xdotool key --clearmodifiers --window "$window" Return

  for _ in $(seq 1 300); do
    if [ -f "$status_path" ]; then
      break
    fi
    sleep 0.1
  done
  if [ ! -f "$status_path" ]; then
    echo "xvfb Codex smoke: Codex negotiation did not complete" >&2
    exit 1
  fi

  probe=$(sed -n "s/^codex_probe=//p" "$status_path")
  keyboard_push=$(sed -n "s/^codex_keyboard_push=//p" "$status_path")
  probe_response=$(sed -n "s/^libtsm_probe_response=//p" "$status_path")
  if [ "$probe" != 1 ] || [ "$keyboard_push" != 1 ] || [ "$probe_response" != 1 ]; then
    echo "xvfb Codex smoke: probe=$probe push=$keyboard_push response=$probe_response" >&2
    sed -n "1,8p" "$status_path" >&2
    exit 1
  fi
  echo "xvfb Codex smoke ok: real Codex negotiated Kitty keyboard through Pragtical"
' xvfb-codex "$test_dir" "$script_dir/codex_app_proxy.py" "$PRAGTICAL_BIN" "$codex_bin"
