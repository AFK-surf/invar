# Source from a CI step: background tasks that outlive the step that starts them.
#   bg_start <label> <command...>  run it detached; its output goes to $RUNNER_TEMP/bg/<label>.log
#   bg_wait <label>                wait for it, print its log, return its exit status
# A shell function given to bg_start runs in a subshell of its own, so a `set -e` it turns on still stops it.
BG_DIR="${RUNNER_TEMP:-/tmp}/bg"
mkdir -p "$BG_DIR"

bg_start() {
  local label=$1
  shift
  rm -f "$BG_DIR/$label.rc"
  (
    set +e
    SECONDS=0
    ("$@") > "$BG_DIR/$label.log" 2>&1 < /dev/null
    rc=$?
    echo "$label ${SECONDS}s rc=$rc" >> "${RUNNER_TEMP:-/tmp}/timings.txt"
    echo "$rc" > "$BG_DIR/$label.rc.tmp"
    mv "$BG_DIR/$label.rc.tmp" "$BG_DIR/$label.rc"
  ) > /dev/null 2>&1 < /dev/null &
}

bg_wait() {
  local label=$1
  while [ ! -f "$BG_DIR/$label.rc" ]; do
    sleep 0.5
  done
  echo "=== $label ==="
  cat "$BG_DIR/$label.log"
  return "$(cat "$BG_DIR/$label.rc")"
}
