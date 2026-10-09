# 新建：linux/amp_hmi.py
#
# 阶段八：Linux Web 控制台 + SQLite 操作日志。
# 使用已验证的 ampctl，实现状态查询、LED 控制和历史记录。
# 只监听 127.0.0.1，通过 SSH 隧道访问。
# 本代码尚未在你的开发板验证。
#
# 阶段七尚缺 RS485 收发器及 DE/RE 接线信息：
# ttyS3 存在并不证明它就是 RS485，暂不操作 UART 寄存器。
#
# 阶段九的 IPC 延迟测量和压力测试后续独立实现。
#
# 阶段十当前不具备 boot_a/boot_b、rootfs_a/rootfs_b；
# 未确认启动槽选择和失败计数机制，暂不提供自动分区/刷写代码。

import argparse
import hmac
import json
import os
import secrets
import sqlite3
import subprocess
import time
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
from urllib.parse import urlsplit

TOKEN = secrets.token_hex(32)
AMPCTL = ""
DATABASE = ""

PAGE = r"""<!doctype html>
<html lang="zh-CN">
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>RK3588 AMP 控制台</title>
<style>
body{max-width:960px;margin:32px auto;padding:0 16px;
font-family:system-ui,sans-serif;background:#f5f7fa;color:#172033}
button,input{padding:10px;margin:4px}
pre{white-space:pre-wrap;overflow-wrap:anywhere;background:white;
padding:16px;border:1px solid #dce2ea;border-radius:8px}
</style>
<h1>RK3588 AMP 控制台</h1>
<p>Linux → IPC → FreeRTOS → 板载 LED</p>
<div>
<button onclick="run('status')">系统状态</button>
<button onclick="run('led_status')">LED 状态</button>
<button onclick="run('ping')">PING</button>
<button onclick="run('led_on')">亮</button>
<button onclick="run('led_off')">灭</button>
</div>
<div>
<label>完整闪烁周期（毫秒）
<input id="period" type="number" min="100" max="10000"
step="20" value="1000"></label>
<button onclick="run('led_blink')">闪烁</button>
</div>
<p>状态由手动查询刷新。执行错误不自动重试；超时不能证明命令未执行。</p>
<h2>执行结果</h2>
<pre id="result">等待操作</pre>
<h2>最近操作</h2>
<button onclick="history()">刷新记录</button>
<pre id="history">等待加载</pre>
<script>
const token = "__TOKEN__";
let busy = false;

async function history() {
  try {
    const r = await fetch("/api/history", {cache:"no-store"});
    if (!r.ok) throw new Error("HTTP " + r.status);
    document.getElementById("history").textContent =
      JSON.stringify(await r.json(), null, 2);
  } catch (e) {
    document.getElementById("history").textContent = String(e);
  }
}

async function run(action) {
  if (busy) return;
  busy = true;
  document.querySelectorAll("button").forEach(b => b.disabled = true);
  const body = {action};
  if (action === "led_blink")
    body.period_ms = Number(document.getElementById("period").value);
  try {
    const r = await fetch("/api/command", {
      method:"POST",
      headers:{"Content-Type":"application/json", "X-AMP-Token":token},
      body:JSON.stringify(body)
    });
    document.getElementById("result").textContent =
      JSON.stringify(await r.json(), null, 2);
    await history();
  } catch (e) {
    document.getElementById("result").textContent =
      String(e) + "\n执行状态未知，请查询状态，不要盲目重试。";
  } finally {
    busy = false;
    document.querySelectorAll("button").forEach(b => b.disabled = false);
  }
}
history();
</script>
</html>
"""


def connect_db():
    return sqlite3.connect(DATABASE, timeout=5)


def initialize_db():
    Path(DATABASE).parent.mkdir(parents=True, exist_ok=True)
    with connect_db() as db:
        db.execute("""
            CREATE TABLE IF NOT EXISTS operations (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                started_at TEXT NOT NULL,
                action TEXT NOT NULL,
                arguments TEXT NOT NULL,
                outcome TEXT NOT NULL,
                returncode INTEGER,
                elapsed_ms REAL,
                output TEXT NOT NULL DEFAULT ''
            )
        """)
        # 上次服务崩溃时未完成记录的动作，不能假定没有执行。
        db.execute("""
            UPDATE operations
            SET outcome = 'UNKNOWN_AFTER_RESTART'
            WHERE outcome = 'RUNNING'
        """)


def command_args(body):
    if not isinstance(body, dict):
        raise ValueError("请求必须是 JSON 对象")

    action = body.get("action")
    commands = {
        "status": ["status"],
        "led_status": ["led", "status"],
        "ping": ["ping", "hmi"],
        "led_on": ["led", "on"],
        "led_off": ["led", "off"],
    }

    if not isinstance(action, str):
        raise ValueError("action 无效")

    if action == "led_blink":
        period = body.get("period_ms")
        if (type(period) is not int or
                not 100 <= period <= 10000 or period % 20 != 0):
            raise ValueError("周期必须为 100～10000 ms，且为 20 的整数倍")
        return action, ["led", "blink", str(period)]

    if action not in commands:
        raise ValueError("不支持的操作")

    return action, commands[action]


def execute(action, args):
    # 先持久化操作意图；数据库不可用时不执行硬件命令。
    with connect_db() as db:
        cursor = db.execute("""
            INSERT INTO operations
                (started_at, action, arguments, outcome)
            VALUES (?, ?, ?, 'RUNNING')
        """, (
            datetime.now(timezone.utc).isoformat(),
            action,
            json.dumps(args),
        ))
        record_id = cursor.lastrowid

    begin = time.monotonic_ns()
    returncode = None

    try:
        completed = subprocess.run(
            [AMPCTL, *args],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=6,
            check=False,
        )
        returncode = completed.returncode
        output = completed.stdout.decode("utf-8", errors="replace")[:16384]
        outcome = "OK" if returncode == 0 else "FAILED_OR_UNKNOWN"
    except subprocess.TimeoutExpired:
        outcome = "TIMEOUT_UNKNOWN"
        output = "客户端超时，命令可能已经执行。请查询设备状态。"
    except OSError as exc:
        outcome = "EXEC_FAILED"
        output = str(exc)

    elapsed_ms = (time.monotonic_ns() - begin) / 1_000_000

    result = {
        "id": record_id,
        "outcome": outcome,
        "returncode": returncode,
        # 包含进程启动、HELLO 和命令应答，不是纯 IPC 延迟。
        "client_total_ms": round(elapsed_ms, 3),
        "output": output,
    }

    try:
        with connect_db() as db:
            db.execute("""
                UPDATE operations
                SET outcome=?, returncode=?, elapsed_ms=?, output=?
                WHERE id=?
            """, (outcome, returncode, elapsed_ms, output, record_id))
    except sqlite3.Error as exc:
        result["log_error"] = str(exc)
        result["warning"] = "操作已尝试执行，但结果日志未保存，请勿自动重试。"

    return result


class Handler(BaseHTTPRequestHandler):
    def setup(self):
        super().setup()
        self.connection.settimeout(10)

    def respond(self, status, data, content_type="application/json"):
        if content_type == "application/json":
            data = json.dumps(data, ensure_ascii=False).encode("utf-8")
        else:
            data = data.encode("utf-8")

        self.send_response(status)
        self.send_header("Content-Type", content_type + "; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("X-Frame-Options", "DENY")
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        path = urlsplit(self.path).path

        if path == "/":
            self.respond(200, PAGE.replace("__TOKEN__", TOKEN), "text/html")
        elif path == "/api/history":
            try:
                with connect_db() as db:
                    db.row_factory = sqlite3.Row
                    rows = db.execute("""
                        SELECT * FROM operations ORDER BY id DESC LIMIT 50
                    """).fetchall()
                self.respond(200, [dict(row) for row in rows])
            except sqlite3.Error:
                self.respond(503, {"error": "日志数据库不可用"})
        else:
            self.respond(404, {"error": "Not found"})

    def do_POST(self):
        if urlsplit(self.path).path != "/api/command":
            self.respond(404, {"error": "Not found"})
            return

        supplied = self.headers.get("X-AMP-Token", "")
        if not hmac.compare_digest(supplied, TOKEN):
            self.respond(403, {"error": "请求令牌无效，请刷新页面"})
            return

        try:
            if self.headers.get("Transfer-Encoding"):
                raise ValueError("不支持 Transfer-Encoding")

            media = self.headers.get("Content-Type", "").split(";")[0]
            if media.strip().lower() != "application/json":
                raise ValueError("需要 application/json")

            length = int(self.headers.get("Content-Length", "0"))
            if not 0 < length <= 1024:
                raise ValueError("请求长度无效")

            raw = self.rfile.read(length)
            if len(raw) != length:
                raise ValueError("请求不完整")

            action, args = command_args(json.loads(raw))
        except (ValueError, UnicodeError) as exc:
            self.respond(400, {"error": str(exc)})
            return

        try:
            result = execute(action, args)
        except sqlite3.Error:
            self.respond(503, {"error": "无法记录操作，命令未执行"})
            return

        self.respond(200, result)


def main():
    global AMPCTL, DATABASE

    parser = argparse.ArgumentParser()
    parser.add_argument("--ampctl", default="/opt/amp/bin/ampctl")
    parser.add_argument("--database", default="/userdata/amp/operations.db")
    parser.add_argument("--port", type=int, default=8088)
    options = parser.parse_args()

    AMPCTL = str(Path(options.ampctl).resolve())
    DATABASE = str(Path(options.database).resolve())

    if not os.path.isfile(AMPCTL) or not os.access(AMPCTL, os.X_OK):
        parser.error("ampctl 不存在或不可执行")

    if not 1 <= options.port <= 65535:
        parser.error("端口无效")

    os.umask(0o077)
    initialize_db()

    # 单线程串行执行命令；ampctl 本身还持有已有 IPC 进程锁。
    server = HTTPServer(("127.0.0.1", options.port), Handler)
    print(f"AMP HMI: http://127.0.0.1:{options.port}", flush=True)

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()