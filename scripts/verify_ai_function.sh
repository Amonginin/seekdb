#!/bin/bash
# 在魔搭 CPU 实例（8核32GB）上编译 seekdb 并验证赛题一的 AI 函数。
#
# 用法：把本脚本和仓库一起放到魔搭，然后
#   bash scripts/verify_ai_function.sh
#
# 注意：魔搭实例「1 小时无操作会触发自动关闭」，长编译必须放 tmux：
#   tmux new -s build
#   bash scripts/verify_ai_function.sh 2>&1 | tee /tmp/build.log
#   # 断了就 tmux attach -t build 回来

set -o pipefail

WORK="${WORK:-/tmp/seekdb-verify}"
BRANCH="${BRANCH:-2026-competition}"
COMMIT="${COMMIT:-1efb0095779a3c9017b4c0bb87923052e37bd4b9}"

echo "=== 1/6 环境 ==="
cat /etc/os-release | head -2
echo "内存: $(free -g | awk '/Mem:/{print $2}') GB, 核心: $(nproc)"
echo "磁盘: $(df -h /tmp | tail -1 | awk '{print $4}') 可用于 /tmp"

# Rust：sql-nio 需要
if ! command -v cargo >/dev/null 2>&1; then
  echo "--- 安装 Rust"
  curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
  source "$HOME/.cargo/env"
fi
rustc --version

echo "=== 2/6 拉代码 ==="
mkdir -p "$WORK"
cd "$WORK"
if [ ! -d seekdb ]; then
  git clone --depth 1 -b "$BRANCH" https://github.com/Amonginin/seekdb.git
  cd seekdb
else
  cd seekdb
  git fetch --depth 1 origin "$BRANCH"
  git checkout FETCH_HEAD
fi
git log --oneline -1
echo "目标 commit: $COMMIT"
git log --oneline -1 | grep -q "${COMMIT:0:7}" && echo "✓ 代码已是目标 commit" || echo "! 代码不是目标 commit，请检查"

echo "=== 3/6 拉依赖（只跑一次，400-500MB） ==="
if [ ! -d deps/obclient ]; then
  ./build.sh release --init
fi

echo "=== 4/6 编译（-j8，内存 32G 足够） ==="
./build.sh release --make -j8
ls -la build_release/src/observer/seekdb

echo "=== 5/6 起测试实例 ==="
cd tools/deploy
rm -rf /tmp/obtest
./obd.sh prepare -p /tmp/obtest
./obd.sh start -p /tmp/obtest

echo "=== 6/6 跑 AI 函数验收 ==="
cd ../mysql_test
# 官方测例（题面给的那份）需要 0.0.0.0:8000 上的 mock AI 服务
cd test_suite/ai_function
python3 include/ai_function_mock.py start 8000
cd ../../..
./mysql-test-run.pl --record \
  --suite=ai_function/t/ai_function_enhancement.test \
  --vardir=/tmp/mtr_var \
  --nowarnings \
  --force

echo
echo "=== 结果 ==="
echo "测例输出在 /tmp/mtr_var/log 里；用下面的命令看差异 ==="
echo "  ls /tmp/mtr_var/log"
echo "  diff -u test_suite/ai_function/r/ai_function_enhancement.result \\"
echo "       /tmp/mtr_var/log/ai_function_enhancement.reject 2>/dev/null"
echo
echo "=== 清理 ==="
cd tools/deploy
./obd.sh stop -p /tmp/obtest || true
./obd.sh destroy -p /tmp/obtest || true
