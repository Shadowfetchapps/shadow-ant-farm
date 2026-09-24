#!/usr/bin/env bash
# Removes the per-user install. Colony checkpoints, logs and settings in
# ~/.local/share/shadow-ant-farm are left in place (delete that folder yourself if you want them gone).
set -euo pipefail
NAME="shadow-ant-farm"
rm -f "$HOME/.local/bin/$NAME" "$HOME/.local/share/applications/$NAME.desktop"
rm -rf "$HOME/.local/opt/$NAME"
find "$HOME/.local/share/icons/hicolor" -name "$NAME.*" -delete 2>/dev/null || true
update-desktop-database "$HOME/.local/share/applications" >/dev/null 2>&1 || true
echo "Removed the program. Your colonies remain in ~/.local/share/$NAME"
