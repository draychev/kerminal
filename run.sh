#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

rm -rf "${repo_root}/build" "${repo_root}/bin"

cmake -S "${repo_root}" -B "${repo_root}/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "${repo_root}/build" -j

"${repo_root}/bin/kerminal"
