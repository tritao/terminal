#!/bin/sh

set -eu

: "${PRAGTICAL_BIN:?set PRAGTICAL_BIN to the rebuilt Pragtical executable}"

test_dir=$(mktemp -d /tmp/pragtical-terminal-xvfb.XXXXXX)
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
  if [ "${XVFB_KEYBOARD_SMOKE_TRACE:-0}" = 1 ]; then
    set -x
  fi
  test_dir=$1
  recorder=$2
  app_bin=$3

  if [ "$LIBTSM_LIBRARY_PATH" != "" ]; then
    LD_LIBRARY_PATH="$LIBTSM_LIBRARY_PATH${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    export LD_LIBRARY_PATH
  fi
  PRAGTICAL_USERDIR="$test_dir/user"
  export PRAGTICAL_USERDIR
  SDL_VIDEO_DRIVER=x11
  SDL_VIDEODRIVER=x11
  SDL_RENDER_DRIVER=software
  LIBGL_ALWAYS_SOFTWARE=1
  export SDL_VIDEO_DRIVER SDL_VIDEODRIVER SDL_RENDER_DRIVER LIBGL_ALWAYS_SOFTWARE

  "$app_bin" >"$test_dir/pragtical.log" 2>&1 &
  app_pid=$!
  cleanup_app() {
    status=$?
    kill "$app_pid" 2>/dev/null || true
    wait "$app_pid" 2>/dev/null || true
    return "$status"
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
    echo "xvfb keyboard smoke: Pragtical window was not created" >&2
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

  output="$test_dir/keyboard.hex"
  ready="$test_dir/keyboard.ready"
  command="python3 '\''$recorder'\'' '\''$output'\'' '\''$ready'\''"
  xdotool type --clearmodifiers --window "$window" --delay 1 "$command"
  xdotool key --clearmodifiers --window "$window" Return

  for _ in $(seq 1 100); do
    if [ -f "$ready" ]; then
      break
    fi
    sleep 0.1
  done
  if [ ! -f "$ready" ]; then
    echo "xvfb keyboard smoke: recorder did not start" >&2
    exit 1
  fi
  sleep 0.3

  xdotool key --clearmodifiers --window "$window" ctrl+Up
  sleep 0.2
  xdotool key --clearmodifiers --window "$window" ctrl+c

  for _ in $(seq 1 100); do
    if [ -f "$output" ]; then
      break
    fi
    sleep 0.1
  done
  expected=1b5b313b35411b5b39393b353a3175
  actual=$(cat "$output" 2>/dev/null || true)
  if [ "$actual" != "$expected" ]; then
    echo "xvfb keyboard smoke: expected $expected, got $actual" >&2
    exit 1
  fi
  echo "xvfb keyboard smoke ok: Pragtical delivered CSI-u keyboard input"
' xvfb-keyboard "$test_dir" "$script_dir/keyboard_recorder.py" "$PRAGTICAL_BIN"
