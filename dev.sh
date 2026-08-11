#!/usr/bin/env bash
# 启动 v86 开发服务器 (端口 8082)
# 每次启动前先结束该端口上所有旧进程

set -e

PORT=8082

# 结束之前占用该端口的所有进程
PIDS=$(lsof -ti :"$PORT" 2>/dev/null || true)
if [ -n "$PIDS" ]; then
    echo " Killing existing process(es) on port $PORT: $PIDS"
    kill -9 $PIDS 2>/dev/null || true
    sleep 0.5
fi

echo " Starting dev server on http://localhost:$PORT ..."
exec python3 -m http.server "$PORT"
