#!/usr/bin/env bash
# Build + test Ldns inside WSL using a native target directory.
#
# Usage: wsl-bed build  [preset]   # configure (if needed) and build
#        wsl-bed test   [preset]   # build then run ctest
#        wsl-bed clean  [preset]
#        wsl-bed shell
#
# The source tree lives on /mnt/c (9p, slow for builds), so we build in
# ~/ldns-build/<preset> and point CMake at the mounted source directory.

set -euo pipefail

ACTION="${1:-test}"
PRESET="${2:-linux-debug}"
SRC="/mnt/c/Code/Ldns"
VCPKG_ROOT="${HOME}/vcpkg"
BUILD_ROOT="${HOME}/ldns-build"
BUILD_DIR="${BUILD_ROOT}/${PRESET}"

export VCPKG_ROOT

configure() {
    local sanitize=OFF
    [[ "${PRESET}" == *asan* ]] && sanitize=ON
    cmake -S "${SRC}" -B "${BUILD_DIR}" \
        -DCMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" \
        -DVCPKG_TARGET_TRIPLET=x64-linux \
        -DCMAKE_BUILD_TYPE=Debug \
        -DLDNS_SANITIZE="${sanitize}" \
        -DLDNS_BUILD_TESTS=ON \
        -G Ninja
}

build() {
    [[ -f "${BUILD_DIR}/build.ninja" ]] || configure
    cmake --build "${BUILD_DIR}" --parallel
}

case "${ACTION}" in
    configure) configure ;;
    build) build ;;
    test)
        build
        ctest --test-dir "${BUILD_DIR}" --output-on-failure
        ;;
    clean) rm -rf "${BUILD_DIR}" ;;
    shell)
        build
        exec bash
        ;;
    *)
        echo "usage: $0 {configure|build|test|clean|shell} [preset]" >&2
        exit 2
        ;;
esac
