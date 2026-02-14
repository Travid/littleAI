#!/usr/bin/env bash
set -euo pipefail

# Install the bundled littleAI OpenClaw skill.
#
# Default: install into ~/.openclaw/skills (shared across agents on this machine)
#
# Usage:
#   bash openclaw_skill/install.sh
#   bash openclaw_skill/install.sh --workspace
#   bash openclaw_skill/install.sh --shared
#   bash openclaw_skill/install.sh --target-dir ~/.openclaw/skills
#   bash openclaw_skill/install.sh --with-venv
#
# Notes:
# - OpenClaw loads skills at session start; restart OpenClaw / start a new chat to pick it up.

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$HERE/littleai"

MODE="shared"
TARGET_DIR=""
WITH_VENV=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --workspace)
      MODE="workspace"; shift ;;
    --shared)
      MODE="shared"; shift ;;
    --target-dir)
      TARGET_DIR="$2"; shift 2 ;;
    --with-venv)
      WITH_VENV=1; shift ;;
    -h|--help)
      sed -n '1,120p' "$0"; exit 0 ;;
    *)
      echo "Unknown arg: $1" >&2
      exit 2
      ;;
  esac
done

if [[ ! -d "$SRC" ]]; then
  echo "Missing source skill folder: $SRC" >&2
  exit 1
fi

if [[ -z "$TARGET_DIR" ]]; then
  if [[ "$MODE" == "workspace" ]]; then
    # Most OpenClaw installs default the workspace here.
    TARGET_DIR="$HOME/.openclaw/workspace/skills"
  else
    TARGET_DIR="$HOME/.openclaw/skills"
  fi
fi

mkdir -p "$TARGET_DIR"

echo "Installing skill from: $SRC"
echo "Into: $TARGET_DIR/littleai"

rm -rf "$TARGET_DIR/littleai"
cp -R "$SRC" "$TARGET_DIR/"

# Ensure scripts are executable
chmod +x "$TARGET_DIR/littleai/scripts/"*.py "$TARGET_DIR/littleai/scripts/ensure_venv.sh" || true

if [[ "$WITH_VENV" == "1" ]]; then
  echo "Creating venv + deps for the skill..."
  bash "$TARGET_DIR/littleai/scripts/ensure_venv.sh"
fi

echo "OK: littleai skill installed."
echo "Next: restart OpenClaw / start a new chat so it loads the new skill."
