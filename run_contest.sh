#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
binary="${CONTEST_ROUTER_BINARY:-$project_dir/build-contest-linux/contest_router}"
if [[ ! -x "$binary" ]]; then
    printf 'Build first: bash "%s/build_contest.sh"\n' "$project_dir" >&2
    exit 1
fi
exec "$binary" "$@"
