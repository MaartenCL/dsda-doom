#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_BIN="${SCRIPT_DIR}/build/dsda-doom"
DEST_DIR="${1:-/data/games/doom/dsda}"
DEST_BIN="${DEST_DIR}/dsda-doom"

if [[ ! -x "${SOURCE_BIN}" ]]; then
	echo "Build output not found or not executable: ${SOURCE_BIN}" >&2
	echo "Run: cmake --build ${SCRIPT_DIR}/build" >&2
	exit 1
fi

if [[ ! -d "${DEST_DIR}" ]]; then
	echo "Destination directory does not exist: ${DEST_DIR}" >&2
	exit 1
fi

if [[ -e "${DEST_BIN}" ]]; then
	cp -a "${DEST_BIN}" "${DEST_BIN}.bak.$(date +%Y%m%d-%H%M%S)"
fi

install -m 0755 "${SOURCE_BIN}" "${DEST_BIN}"
echo "Deployed ${SOURCE_BIN} -> ${DEST_BIN}"