#!/usr/bin/env bash
set -euo pipefail

# Clean local/generated artifacts so the folder is ready to publish to GitHub.
# This script moves large/generated directories to macOS Trash for safety.
#
# Usage:
#   bash tools/clean_for_github.sh

cd "$(dirname "${BASH_SOURCE[0]}")/.."

TRASH="$HOME/.Trash/littleAI-github-cleanup-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$TRASH"

TO_MOVE=(
  .pio
  managed_components
  .venv-ws
  dependencies.lock
  waveshare_ref
  .vscode/c_cpp_properties.json
  .vscode/launch.json
)

moved_any=0
for p in "${TO_MOVE[@]}"; do
  if [[ -e "$p" ]]; then
    echo "Moving $p -> $TRASH/"
    mv "$p" "$TRASH/"
    moved_any=1
  fi
done

if [[ "$moved_any" == "0" ]]; then
  echo "Nothing to clean."
else
  echo "OK. Moved items to: $TRASH"
fi

echo
echo "Next steps (suggested):"
echo "  git init"
echo "  git add -A"
echo "  git status"
