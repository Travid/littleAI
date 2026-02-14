#!/usr/bin/env bash
set -euo pipefail

# Creates a local venv for the skill and installs Python deps.
# Usage:
#   bash {baseDir}/scripts/ensure_venv.sh

SKILL_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VENV="$SKILL_DIR/.venv"

if [[ ! -d "$VENV" ]]; then
  python3 -m venv "$VENV"
fi

source "$VENV/bin/activate"
python -m pip install -U pip >/dev/null
python -m pip install websockets==12.0

echo "OK: venv ready at $VENV"
