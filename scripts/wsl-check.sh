#!/usr/bin/env bash
# Diagnostic helper: show whether a build is running and where it is.
set -uo pipefail

echo "== processes"
ps -eo pid,etime,comm,args | grep -E 'vcpkg|ninja|cmake|cc1plus|make' | grep -v grep | head -15 || true

echo "== build dir"
du -sh "${HOME}/ldns-build/linux-debug" 2>/dev/null || echo "missing"

echo "== installed libs"
ls "${HOME}/ldns-build/linux-debug/vcpkg_installed/x64-linux/lib" 2>/dev/null || echo "none"
