#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="${1:-$project_dir/build-contest-linux}"
mkdir -p -- "$build_dir"
sources=(tokenizer lef_parser def_parser pin_resolver gcell_grid astar net_router def_writer
         contest_resources contest_router contest_guide contest_main)
files=()
for source in "${sources[@]}"; do files+=("$project_dir/src/$source.cpp"); done
"${CXX:-g++}" -std=c++17 -O3 -DNDEBUG -Wall -Wextra -Wpedantic \
    -static-libstdc++ -static-libgcc -I"$project_dir/include" \
    "${files[@]}" -o "$build_dir/contest_router"
"$build_dir/contest_router" --version
printf 'Built: %s/contest_router\n' "$build_dir"
