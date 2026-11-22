#!/usr/bin/env bash
set -euo pipefail

PIONC="${PIONC:-pionc}"
if [ "$#" -gt 0 ]; then
  PKGS=("$@")
else
  PKGS=("packages/ui")
fi

RATES=(60 120 24 7)
STATUS=()
FAILED=0

echo "==> Invar multi-FPS determinism check (${RATES[*]} fps) on: ${PKGS[*]}"
for rate in "${RATES[@]}"; do
  echo ""
  echo "--- INVAR_TEST_FPS=${rate} ---"
  if INVAR_TEST_FPS="${rate}" "${PIONC}" test "${PKGS[@]}"; then
    STATUS+=("PASS")
  else
    STATUS+=("FAIL")
    FAILED=1
  fi
done

echo ""
echo "=== FPS Check Summary ==="
for i in "${!RATES[@]}"; do
  printf "  %3s fps : %s\n" "${RATES[$i]}" "${STATUS[$i]}"
done

exit "${FAILED}"
