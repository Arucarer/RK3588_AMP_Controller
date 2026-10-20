#!/bin/sh
# Ubuntu host: build Linux ARM64 client, never deploy or execute on the board.
set -eu
if [ "$#" -gt 1 ]; then
    echo "usage: $0 [output-path]" >&2
    exit 2
fi
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
sdk_dir=${SDK:-"$HOME/rk3588_linux_sdk"}
cross_prefix=${CROSS_COMPILE:-"$sdk_dir/prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-"}
output=${1:-"$project_dir/build/linux/amp_motorctl"}
mkdir -p "$(dirname -- "$output")"
"${cross_prefix}gcc" -std=c11 -O2 -Wall -Wextra -Werror -static \
    "$project_dir/linux/amp_motorctl.c" "$project_dir/linux/amp_ipc.c" \
    "$project_dir/common/amp_message.c" "$project_dir/common/amp_transport.c" \
    -o "$output"
echo "Created: $output"
