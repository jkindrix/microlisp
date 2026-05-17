#!/usr/bin/env bash
# Point this repository's Git config at the in-tree hooks under .githooks/.
# Run once after cloning. Idempotent.
set -euo pipefail

cd "$(dirname "$0")/.."

git config core.hooksPath .githooks
chmod +x .githooks/*

echo "installed: core.hooksPath = $(git config --get core.hooksPath)"
