#!/usr/bin/env bash
# Install the unified `invar` CLI by symlinking `scripts/invar` into
# `${INVAR_BIN_DIR:-$HOME/.pion/bin}/invar` (with fallback hint for `~/.local/bin`).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INVAR_SRC="$SCRIPT_DIR/invar"

if [ ! -x "$INVAR_SRC" ]; then
  chmod +x "$INVAR_SRC"
fi

BIN_DIR="${INVAR_BIN_DIR:-$HOME/.pion/bin}"
mkdir -p "$BIN_DIR"
TARGET="$BIN_DIR/invar"
ln -sfn "$INVAR_SRC" "$TARGET"

echo "Installed invar -> $TARGET (symlink to $INVAR_SRC)"

case ":${PATH:-}:" in
  *":$BIN_DIR:"*)
    echo "PATH already includes $BIN_DIR. Run 'invar doctor' to verify your toolchain."
    ;;
  *)
    echo "Add $BIN_DIR to your PATH (or symlink into ~/.local/bin):"
    echo "  export PATH=\"$BIN_DIR:\$PATH\""
    ;;
esac
