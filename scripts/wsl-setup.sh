#!/usr/bin/env bash
# One-time WSL setup: move any stray vcpkg clone into place and bootstrap it.
set -euo pipefail

HOME_DIR="${HOME}"
TARGET="${HOME_DIR}/vcpkg"
STRAY="${HOME_DIR}/C:Usersdthan/vcpkg"

if [[ ! -d "${TARGET}" && -d "${STRAY}" ]]; then
    mv "${STRAY}" "${TARGET}"
fi

if [[ ! -x "${TARGET}/vcpkg" ]]; then
    if [[ ! -d "${TARGET}" ]]; then
        git clone https://github.com/microsoft/vcpkg.git "${TARGET}"
    fi
    "${TARGET}/bootstrap-vcpkg.sh" -disableMetrics
fi

"${TARGET}/vcpkg" version | head -2
git -C "${TARGET}" rev-parse HEAD
