#!/usr/bin/env bash
set -euo pipefail

repo="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)"
build="$repo/build/remote-gcc"
cd -- "$repo"

if [[ ! -f "$build/CMakeCache.txt" && -z "${CUDACXX:-}" ]] && ! command -v nvcc >/dev/null; then
    for nvcc in /usr/local/cuda-12/bin/nvcc /usr/local/cuda/bin/nvcc; do
        if [[ -x "$nvcc" ]]; then
            export CUDACXX="$nvcc"
            break
        fi
    done
fi

cmake -S "$repo" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DFALCOR_ENABLE_REMOTE=ON
cmake --build "$build" --target FalcorRemoteServer --parallel "${FALCOR_BUILD_JOBS:-8}"
exec "$build/bin/FalcorRemoteServer" "$@"
