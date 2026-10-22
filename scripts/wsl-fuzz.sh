#!/usr/bin/env bash
# Build and run the DNS parser fuzz harness inside WSL (Linux).
#
# Usage: wsl-fuzz.sh [seconds] [harness]
#   seconds  default 60
#   harness  default dns_message; also: name
#
# Clang is used because GCC has no libFuzzer; ASan+UBSan are enabled so memory
# bugs surface as crashes.

set -euo pipefail

SECONDS_TO_RUN="${1:-60}"
HARNESS="${2:-dns_message}"
SRC="/mnt/c/Code/Ldns"
OUT="${HOME}/ldns-fuzz"
mkdir -p "${OUT}/corpus/${HARNESS}"
# Seed with committed regression inputs (tests/fuzz/corpus/*).
find "${SRC}/tests/fuzz/corpus" -type f -name "*.bin" -exec cp -n {} "${OUT}/corpus/${HARNESS}/" \; 2>/dev/null || true

case "${HARNESS}" in
    dns_message)
        SOURCE="${SRC}/tests/fuzz/fuzz_dns_message.cpp"
        LIBS="${SRC}/src/core/dns/message.cpp ${SRC}/src/core/dns/name.cpp"
        ;;
    dns_name)
        SOURCE="${SRC}/tests/fuzz/fuzz_dns_name.cpp"
        LIBS="${SRC}/src/core/dns/name.cpp"
        ;;
    *)
        echo "unknown harness: ${HARNESS}" >&2
        exit 2
        ;;
esac

# shellcheck disable=SC2086  # LIBS is an intentional word list
clang++ -std=c++20 -g -O1 -fno-omit-frame-pointer \
    -fsanitize=fuzzer,address,undefined \
    -I"${SRC}/src" \
    "${SOURCE}" ${LIBS} \
    -o "${OUT}/fuzz_${HARNESS}"

"${OUT}/fuzz_${HARNESS}" \
    -max_total_time="${SECONDS_TO_RUN}" \
    -max_len=4096 \
    -timeout=5 \
    -print_final_stats=1 \
    "${OUT}/corpus/${HARNESS}"
