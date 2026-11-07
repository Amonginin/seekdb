#!/bin/bash
# 在魔搭 CPU 实例上编译 seekdb 并验证赛题一的 AI 函数。
#
# 用法（不需要先 cd 到任何目录）：
#   bash scripts/verify_ai_function.sh
#
# 长编译请放 tmux，防止实例被回收：
#   tmux new -s build
#   bash scripts/verify_ai_function.sh 2>&1 | tee /tmp/build.log

set -o pipefail

# 已经在 seekdb 源码目录里就直接用它，否则用工作区里那个
if [ -f "./build.sh" ] && [ -d "./src/sql/engine/expr/ob_expr_ai" ]; then
  SRC="$(pwd)"
elif [ -f "/mnt/workspace/seekdb/build.sh" ]; then
  SRC="/mnt/workspace/seekdb"
else
  SRC="${SRC:-/mnt/workspace/seekdb}"
fi
BRANCH="${BRANCH:-2026-competition}"
COMMIT="${COMMIT:-24063bab5cb866c678e142c7c87cc3ea5c871cdc}"
JOBS="${JOBS:-8}"

echo "=== 1/6 环境 ==="
echo "源码目录: $SRC"
cat /etc/os-release | grep PRETTY | head -1
echo "内存: $(free -g | awk '/Mem:/{print $2}') GB   核心: $(nproc)"
echo "/tmp 可用: $(df -h /tmp | tail -1 | awk '{print $4}')   /mnt/workspace 可用: $(df -h /mnt/workspace | tail -1 | awk '{print $4}')"
if ! command -v cargo >/dev/null 2>&1; then
  echo "--- 安装 Rust（sql-nio 需要）"
  curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
  source "$HOME/.cargo/env"
fi
rustc --version

echo
echo "=== 2/6 同步代码 ==="
cd "$SRC"
# 若远端需要凭据，用环境变量提供，不要把令牌写进脚本
if [ -n "${GITHUB_TOKEN:-}" ]; then
  git remote set-url origin "https://Amonginin:${GITHUB_TOKEN}@github.com/Amonginin/seekdb.git"
fi
git fetch origin "$BRANCH" 2>&1 | tail -2
git checkout "$BRANCH" 2>&1 | tail -1
git reset --hard "origin/$BRANCH" 2>&1 | tail -1
git log --oneline -1
echo "目标 commit: $COMMIT"

echo
echo "=== 3/6 依赖（已有则跳过） ==="
if [ -d deps/obclient ] && [ -d deps/obclient/src ]; then
  echo "deps 已就绪，跳过 --init"
else
  echo "拉依赖（400-500MB，几分钟）..."
  ./build.sh release --init
fi

echo
echo "=== 4/6 编译 ==="
if [ -d build_release ]; then
  echo "build_release 已存在，CMake 会做增量编译（只重编改动的文件）"
  cd build_release
  MAKE_OPTS="-j${JOBS}"
  # CMakeLists 变了就要重新 configure
  if [ CMakeLists.txt -nt CMakeCache.txt ]; then
    echo "CMakeLists 有变化，重新 configure"
    cd "$SRC" && ./build.sh release --make "$MAKE_OPTS"
  else
    make $MAKE_OPTS
  fi
else
  cd "$SRC"
  ./build.sh release --make "-j${JOBS}"
fi
ls -la "$SRC"/build_release/src/observer/seekdb

echo
echo "=== 5/6 起测试实例 ==="
cd "$SRC/tools/deploy"
rm -rf /tmp/obtest
./obd.sh prepare -p /tmp/obtest
./obd.sh start -p /tmp/obtest

echo
echo "=== 6/6 跑 AI 函数验收 ==="
cd "$SRC/tools/deploy/mysql_test"
python3 test_suite/ai_function/include/ai_function_mock.py start 8000
./mysql-test-run.pl --record \
  --suite=ai_function/t/ai_function_enhancement.test \
  --vardir=/tmp/mtr_var \
  --nowarnings \
  --force

echo
echo "=== 结果 ==="
echo "测例输出："
ls /tmp/mtr_var/log 2>/dev/null | head -20
echo
echo "期望结果文件："
ls test_suite/ai_function/r/ 2>/dev/null
echo
echo "看差异："
echo "  diff -u test_suite/ai_function/r/ai_function_enhancement.result \\"
echo "       /tmp/mtr_var/log/ai_function_enhancement.reject 2>/dev/null || true"
echo
echo "=== 清理 ==="
cd "$SRC/tools/deploy"
./obd.sh stop -p /tmp/obtest || true
./obd.sh destroy -p /tmp/obtest || true
