#!/usr/bin/env bash
# Build + test Ldns inside WSL using a native target directory.
#
# Usage: wsl-build.sh build  [preset]   # configure (if needed) and build
#        wsl-build.sh test   [preset]   # build then run ctest
#        wsl-build.sh clean  [preset]
#        wsl-build.sh shell
#
# The source tree lives on /mnt/c (9p, slow for builds), so we build in
# ~/<project>-build/<preset> and point CMake at the mounted source directory.

set -euo pipefail

ACTION="${1:-test}"
PRESET="${2:-linux-debug}"
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJECT_NAME="$(basename "${SRC}")"
VCPKG_ROOT="${HOME}/vcpkg"
BUILD_ROOT="${HOME}/${PROJECT_NAME}-build"
BUILD_DIR="${BUILD_ROOT}/${PRESET}"

export VCPKG_ROOT

configure() {
    local sanitize=OFF
    local build_type=Debug
    [[ "${PRESET}" == *asan* ]] && sanitize=ON
    [[ "${PRESET}" == *release* ]] && build_type=Release
    cmake -S "${SRC}" -B "${BUILD_DIR}" \
        -DCMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" \
        -DVCPKG_TARGET_TRIPLET=x64-linux \
        -DCMAKE_BUILD_TYPE="${build_type}" \
        -DLDNS_SANITIZE="${sanitize}" \
        -DLDNS_BUILD_TESTS=ON \
        -G Ninja
}

needs_configure() {
    [[ ! -f "${BUILD_DIR}/build.ninja" ]] && return 0
    if [[ -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
        local cached_src
        cached_src=$(grep "CMAKE_HOME_DIRECTORY:INTERNAL=" "${BUILD_DIR}/CMakeCache.txt" | cut -d= -f2 || true)
        [[ "${cached_src}" != "${SRC}" ]] && return 0
    fi
    return 1
}

build() {
    if needs_configure; then
        configure
    fi
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
        echo "Unknown action: ${ACTION}" >&2
        exit 1
        ;;
esac
