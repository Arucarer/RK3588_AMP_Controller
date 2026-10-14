#!/bin/sh
set -eu

# 1. 数据库必须写入已经挂载的持久化 userdata。
# 挂载缺失时停止，避免误写到 rootfs 下的同名目录。
if ! awk '
    $2 == "/userdata" { found = 1 }
    END { exit(found ? 0 : 1) }
' /proc/mounts; then
    echo "ERROR: /userdata 未挂载" >&2
    exit 1
fi

# 2. 核对目录权限、应用文件和 Python 依赖。
test -w /userdata
test -x /opt/amp/bin/ampctl
test -r /opt/amp/amp_hmi.py
command -v python3 >/dev/null

# 3. 限制日志数据库权限，并创建持久化目录。
umask 077
mkdir -p /userdata/amp

# 依赖缺失时，给出 Python 错误并停止。
python3 -c 'import sqlite3, http.server'

# 4. 沿用当前人工启动 FreeRTOS 的流程。
# IPC 未就绪时停止，不自动重置共享区。
/opt/amp/bin/ampctl status

# 5. 启动控制台，使用 userdata 保存操作记录。
exec python3 /opt/amp/amp_hmi.py \
    --ampctl /opt/amp/bin/ampctl \
    --database /userdata/amp/operations.db \
    --port 8088
