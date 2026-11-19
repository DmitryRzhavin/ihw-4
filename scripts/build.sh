#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
if command -v cmake >/dev/null; then
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
    cmake --build build --parallel
else
    echo 'CMake not found; building directly with GCC.'
    mkdir -p build
    for name in server client observer; do
        gcc -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wpedantic -Werror -g -Iinclude \
            "src/$name.c" src/model.c src/config.c src/net.c -o "build/kitchen_$name"
    done
    gcc -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wpedantic -Werror -g -Iinclude \
        tests/model_test.c src/model.c -o build/model_test
fi
