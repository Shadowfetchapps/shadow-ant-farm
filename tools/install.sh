#!/usr/bin/env bash
# Installs Shadow Ant Farm for the current user only (no root): ~/.local/opt, ~/.local/bin,
# ~/.local/share/applications and the hicolor icon theme. Run tools/build.sh first.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NAME="shadow-ant-farm"
SRC="$ROOT/dist/$NAME"
OPT="$HOME/.local/opt/$NAME"
BIN="$HOME/.local/bin"
APPS="$HOME/.local/share/applications"
ICONS="$HOME/.local/share/icons/hicolor"
[[ -x "$SRC/$NAME.x86_64" ]] || { echo "Build first: tools/build.sh" >&2; exit 1; }
mkdir -p "$OPT" "$BIN" "$APPS" "$ICONS/scalable/apps"
# Replace the program files only; the colony checkpoints and settings live in ~/.local/share/shadow-ant-farm.
cp -f "$SRC/$NAME.x86_64" "$SRC/libantfarm.so" "$OPT/"
cp -f "$ROOT/packaging/$NAME.svg" "$OPT/"
chmod +x "$OPT/$NAME.x86_64"
cat > "$BIN/$NAME" <<LAUNCH
#!/bin/sh
# Shadow Ant Farm launcher. Options: --resume, --seed N, --windowed, --operator, --fps 30|60,
# --soak HOURS, --capture DIR. See the operating guide.
exec "$OPT/$NAME.x86_64" -- "\$@"
LAUNCH
chmod +x "$BIN/$NAME"
cp -f "$ROOT/packaging/$NAME.svg" "$ICONS/scalable/apps/$NAME.svg"
if command -v rsvg-convert >/dev/null 2>&1; then
	for s in 16 24 32 48 64 128 256 512; do
		mkdir -p "$ICONS/${s}x${s}/apps"
		rsvg-convert -w "$s" -h "$s" "$ROOT/packaging/$NAME.svg" -o "$ICONS/${s}x${s}/apps/$NAME.png"
	done
fi
sed "s|@BIN@|$BIN/$NAME|g" "$ROOT/packaging/$NAME.desktop" > "$APPS/$NAME.desktop"
command -v desktop-file-validate >/dev/null 2>&1 && desktop-file-validate "$APPS/$NAME.desktop" || true
update-desktop-database "$APPS" >/dev/null 2>&1 || true
gtk-update-icon-cache -q "$ICONS" >/dev/null 2>&1 || true
echo "Installed: $BIN/$NAME (program in $OPT)"
