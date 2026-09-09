#!/usr/bin/env bash

set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${repo_dir}/.build/ui-preview"
variant="${1:-utility}"
state="${2:-home}"

cmake_args=()
if [[ -n "${FETCHCONTENT_SOURCE_DIR_LVGL:-}" ]]; then
  cmake_args+=("-DFETCHCONTENT_SOURCE_DIR_LVGL=${FETCHCONTENT_SOURCE_DIR_LVGL}")
fi

if [[ -z "${SDL_VIDEODRIVER:-}" \
   && -z "${DISPLAY:-}" \
   && -z "${WAYLAND_DISPLAY:-}" ]]; then
  export SDL_VIDEODRIVER=dummy
fi

cmake -S "${repo_dir}/simulator" -B "${build_dir}" -G Ninja "${cmake_args[@]}"
cmake --build "${build_dir}" --target unit-lcd-ui-sim
exec "${build_dir}/unit-lcd-ui-sim" --variant "${variant}" --state "${state}"
