#!/usr/bin/env bash
# 魔搭 DSW 实例重建脚本（seekdb 编译环境）
#
# 用法（在魔搭终端执行）：
#   bash /tmp/seekdb/tools/comp_recover.sh check    # 体检：NAS 备份、源码、工具链
#   bash /tmp/seekdb/tools/comp_recover.sh deps     # 装依赖（rpmfile 解 rpm + rust）
#   bash /tmp/seekdb/tools/comp_recover.sh build    # 编译 release
#   bash /tmp/seekdb/tools/comp_recover.sh test     # 起 mock + 起库 + 跑 B1/B3 测例
#
# 设计原则：
#   1. 每个阶段可独立重复执行（幂等），失败可单段重跑
#   2. 长命令不靠粘贴，全部写在本文件里
#   3. 所有输出落日志文件，终端只打结论

set -uo pipefail

SRC_DIR="${SRC_DIR:-/tmp/seekdb}"
STAGE="${1:-help}"
LOG_DIR="${LOG_DIR:-/tmp/recover}"
mkdir -p "${LOG_DIR}"

log()  { printf '\n=== %s ===\n' "$*"; }
info() { printf '  %s\n' "$*"; }
die()  { printf '  ✗ %s\n' "$*" >&2; exit 1; }

DEVTOOLS="${SRC_DIR}/deps/3rd/usr/local/oceanbase/devtools"
export PATH="${DEVTOOLS}/bin:${PATH}"

# ---------------------------------------------------------------- check
stage_check() {
  log "体检"

  info "--- NAS 备份（/mnt/workspace）---"
  if [ -d /mnt/workspace ]; then
    ls -lh /mnt/workspace/*.tgz 2>/dev/null || info "(没有 tgz 备份)"
    df -h /mnt/workspace 2>/dev/null | tail -1
  else
    info "/mnt/workspace 不存在（不是魔搭 DSW？）"
  fi

  info "--- 本地盘 ---"
  df -h /tmp | tail -1

  info "--- 源码 ${SRC_DIR} ---"
  if [ -d "${SRC_DIR}/.git" ]; then
    (cd "${SRC_DIR}" && git log --oneline -1 && git status --short | head -5)
  else
    info "源码不在（需要 clone）"
  fi

  info "--- 工具链 ---"
  for t in bison clang clang++ cmake cargo make; do
    if command -v "$t" >/dev/null 2>&1; then
      printf '  %-8s %s\n' "$t" "$(command -v $t)"
    else
      printf '  %-8s ✗ 缺失\n' "$t"
    fi
  done
  bison -V 2>/dev/null | head -1
  clang --version 2>/dev/null | head -1

  info "--- mysqltest ---"
  find_mysqltest

  info "--- 产物 ---"
  [ -x "${SRC_DIR}/build_release/src/observer/seekdb" ] \
    && echo "  ✓ build_release/src/observer/seekdb 已存在" \
    || echo "  ✗ 尚未编译"
}

find_mysqltest() {
  local c
  for c in "${SRC_DIR}/deps/3rd/u01/obclient/bin/mysqltest" \
           "${SRC_DIR}/deps/3rd/usr/local/oceanbase/client/bin/mysqltest" \
           "$(command -v mysqltest 2>/dev/null)"; do
    if [ -n "${c}" ] && [ -x "${c}" ]; then
      MYSQLTEST="${c}"
      echo "  mysqltest: ${MYSQLTEST}"
      return 0
    fi
  done
  echo "  mysqltest: ✗ 未找到"
  MYSQLTEST=""
  return 1
}

# ---------------------------------------------------------------- deps
stage_deps() {
  log "阶段 1/3：依赖"

  cd "${SRC_DIR}" || die "找不到源码目录 ${SRC_DIR}"

  # 1a. 源码必须在位
  if [ ! -f build.sh ]; then
    die "源码不在 ${SRC_DIR}。先执行：
  git clone -b 2026-competition https://gh-proxy.com/https://github.com/Amonginin/seekdb.git ${SRC_DIR}"
  fi

  # 1b. 直接解包已有备份（最快）
  if [ -f /mnt/workspace/deps3rd.tgz ]; then
    info "发现 /mnt/workspace/deps3rd.tgz，直接解包"
    tar xzf /mnt/workspace/deps3rd.tgz -C "${SRC_DIR}" || die "解包失败"
  fi

  # 1b-1. dep_create.sh 会 rm -rf deps/3rd 后重下，只有这两个标记能让它早退。
  # 缺了标记就会在编译阶段重下 29 个 rpm，表现为 CPU 0% 且长时间无进展。
  local dep_md5
  dep_md5=$(md5sum deps/init/oceanbase.el9.x86_64.deps | cut -d' ' -f1)
  touch "deps/3rd/DONE" "deps/3rd/${dep_md5}"
  info "已打 deps/3rd/DONE 标记"

  # 1c. 工具链自检
  if [ ! -x "${DEVTOOLS}/bin/clang++" ] && [ ! -x "${DEVTOOLS}/bin/clang++-17" ]; then
    info "解 rpm 依赖（apt 源 403，装不上 rpm2cpio，用 rpmfile）"
    python3 -m pip install --quiet rpmfile zstandard 2>&1 | tail -2
    mkdir -p /tmp/rpmpkgs

    python3 - <<'PY'
import os, subprocess
from concurrent.futures import ThreadPoolExecutor

DEV  = "http://mirrors.oceanbase.com/oceanbase/development-kit/el/8/x86_64/"
COMM = "http://mirrors.oceanbase.com/oceanbase/community/stable/el/8/x86_64/"
DEP  = "deps/init/oceanbase.el9.x86_64.deps"

pkgs = []
for ln in open(DEP, encoding="utf-8"):
    ln = ln.strip()
    if not ln or ln.startswith("#"):
        continue
    if ln.startswith("["):
        continue
    if ln.startswith(("os=", "arch=", "repo=")):
        continue
    p = ln.split()
    if not p or not p[0].endswith(".rpm"):
        continue
    repo = COMM if len(p) > 1 and p[1] == "target=community" else DEV
    pkgs.append((repo + p[0], "/tmp/rpmpkgs/" + p[0]))

def dl(t):
    url, out = t
    if os.path.exists(out) and os.path.getsize(out) > 0:
        return "cached " + os.path.basename(out)
    r = subprocess.run(["curl", "-L", "-f", "-s", "--retry", "3", "-o", out, url])
    return ("OK   " if r.returncode == 0 else "FAIL ") + os.path.basename(out)

with ThreadPoolExecutor(max_workers=8) as ex:
    for res in ex.map(dl, pkgs):
        print(res, flush=True)
print("total:", len(pkgs))
PY

    local cnt
    cnt=$(ls /tmp/rpmpkgs/*.rpm 2>/dev/null | wc -l)
    info "已下载 ${cnt} 个 rpm"
    [ "${cnt}" -ge 29 ] || die "rpm 数量不足（期望 29，实际 ${cnt}）"

    local f
    for f in /tmp/rpmpkgs/*.rpm; do
      rpmfile -xvC "${SRC_DIR}/deps/3rd" "$f" >/dev/null 2>&1 || echo "  FAIL $f"
    done

    # dep_create.sh 会 rm -rf deps/3rd，必须打标记让它提前 exit 0
    local md5
    md5=$(md5sum deps/init/oceanbase.el9.x86_64.deps | cut -d' ' -f1)
    touch "deps/3rd/DONE" "deps/3rd/${md5}"
    info "已打 deps/3rd/DONE 标记"
  else
    info "deps/3rd 已就绪"
  fi

  # 1d. 编译器软链（rpmfile 未必还原符号链接）
  mkdir -p "${DEVTOOLS}/bin"
  [ -e "${DEVTOOLS}/bin/clang-17" ]   || ln -s clang   "${DEVTOOLS}/bin/clang-17"
  [ -e "${DEVTOOLS}/bin/clang++-17" ] || ln -s clang++ "${DEVTOOLS}/bin/clang++-17"

  # 1e. Rust 1.98.1
  export PATH="$HOME/.cargo/bin:${PATH}"
  if ! command -v cargo >/dev/null 2>&1 || ! cargo --version 2>/dev/null | grep -q "1\.98\.1"; then
    info "装 Rust 1.98.1"
    export RUSTUP_DIST_SERVER=https://rsproxy.cn
    export RUSTUP_UPDATE_ROOT=https://rsproxy.cn/rustup
    curl --proto '=https' --tlsv1.2 -sSf https://rsproxy.cn/rustup-init.sh \
      | sh -s -- -y --default-toolchain none >/dev/null 2>&1
    [ -f "$HOME/.cargo/env" ] && . "$HOME/.cargo/env"
    export PATH="$HOME/.cargo/bin:${PATH}"
    # rust-toolchain.toml 钉了 5 个 target（含 iOS）。少装任何一个，编译时
    # rustup 会去补装，而 aarch64-apple-ios 的 std 组件会冲突失败。
    rustup toolchain install 1.98.1 --profile minimal --component clippy \
      --target x86_64-unknown-linux-gnu \
      --target x86_64-pc-windows-gnu \
      --target aarch64-linux-android \
      --target aarch64-apple-ios \
      --target aarch64-apple-ios-sim >/dev/null 2>&1
    rustup default 1.98.1 >/dev/null 2>&1
    mkdir -p "$HOME/.cargo"
    cat > "$HOME/.cargo/config.toml" <<'EOF'
[source.rsproxy-sparse]
registry = "sparse+https://rsproxy.cn/index/"
[source.crates-io]
replace-with = "rsproxy-sparse"
[net]
git-fetch-with-cli = true
EOF
  else
    info "Rust 已就绪"
  fi

  log "依赖自检"
  bison -V  2>/dev/null | head -1 || die "bison 缺失（必须 2.4.1）"
  clang --version 2>/dev/null | head -1 || die "clang 缺失"
  cmake --version 2>/dev/null | head -1 || die "cmake 缺失"
  cargo --version 2>/dev/null || die "cargo 缺失"

  # 1f. toolchain 已在，但 target 可能不全。少一个 iOS target，
  # 编译时 rustup 会补装并撞上组件冲突。
  if command -v rustup >/dev/null 2>&1; then
    local missing_targets=""
    for t in x86_64-unknown-linux-gnu x86_64-pc-windows-gnu \
             aarch64-linux-android aarch64-apple-ios aarch64-apple-ios-sim; do
      if ! rustup target list --installed --toolchain 1.98.1 2>/dev/null | grep -qx "$t"; then
        missing_targets="${missing_targets} ${t}"
      fi
    done
    if [ -n "${missing_targets}" ]; then
      info "补装缺失的 rust target:${missing_targets}"
      export RUSTUP_DIST_SERVER=https://rsproxy.cn
      export RUSTUP_UPDATE_ROOT=https://rsproxy.cn/rustup
      # shellcheck disable=SC2086
      rustup target add --toolchain 1.98.1 ${missing_targets} >/dev/null 2>&1 \
        || die "rust target 补装失败：${missing_targets}"
    fi
    rustup default 1.98.1 >/dev/null 2>&1
  fi
  info "✓ 依赖就绪"
}

# ---------------------------------------------------------------- build
stage_build() {
  log "阶段 2/3：编译（release）"
  cd "${SRC_DIR}" || die "找不到 ${SRC_DIR}"

  # 拉最新代码（含新增文件；不动分支历史）
  git fetch origin 2026-competition 2>&1 | tail -2
  git checkout FETCH_HEAD -- src/ tools/ deps/ 2>/dev/null || true
  git log --oneline -1

  pkill -f "seekdb --base-dir" 2>/dev/null; sleep 2

  # tee：屏幕上直接看进度，同时留一份日志备查。
  # 退出码取 PIPESTATUS[0]，$? 会拿到 tee 的返回值。
  ./build.sh release --make -j8 2>&1 | tee "${LOG_DIR}/build.log"
  local rc=${PIPESTATUS[0]}
  echo "build exit=${rc}"
  if [ ${rc} -ne 0 ]; then
    grep -nE "error:|fatal error|Error " "${LOG_DIR}/build.log" | head -20
    die "编译失败，日志：${LOG_DIR}/build.log"
  fi
  [ -x "${SRC_DIR}/build_release/src/observer/seekdb" ] || die "产物不存在"
  info "✓ 编译完成：build_release/src/observer/seekdb"
}

# ---------------------------------------------------------------- test
stage_test() {
  log "阶段 3/3：跑测例"

  # mock 服务
  pkill -f ai_column_mock.py 2>/dev/null; sleep 1
  pkill -f ai_function_mock.py 2>/dev/null; sleep 1
  cd "${SRC_DIR}/tools/deploy/mysql_test" || die "找不到 mysql_test"
  python3 test_suite/ai_column/include/ai_column_mock.py start 19120 > "${LOG_DIR}/mock.log" 2>&1
  python3 test_suite/ai_function/include/ai_function_mock.py start 8000 >> "${LOG_DIR}/mock.log" 2>&1
  sleep 2
  info "✓ mock 已启动（ai_column:19120, ai_function:8000）"

  # 起库
  rm -rf /tmp/sdb && mkdir -p /tmp/sdb
  local bin="${SRC_DIR}/build_release/src/observer/seekdb"
  if [ ! -x "${bin}" ]; then
    die "二进制不存在：${bin}。先跑 bash $0 build"
  fi
  "${bin}" --base-dir /tmp/sdb --port 2881 --nodaemon > /tmp/sdb/server.log 2>&1 &
  local server_pid=$!

  # 等端口真的可连，不要固定 sleep，实例可能起得比 20 秒慢。
  local waited=0
  local up=0
  while [ ${waited} -lt 90 ]; do
    if (exec 3<>/dev/tcp/127.0.0.1/2881) 2>/dev/null; then
      up=1
      break
    fi
    if ! kill -0 ${server_pid} 2>/dev/null; then
      break
    fi
    sleep 2
    waited=$((waited + 2))
  done
  if [ ${up} -ne 1 ]; then
    info "实例没起来，server.log 末尾："
    tail -40 /tmp/sdb/server.log
    die "实例启动失败（等了 ${waited}s）"
  fi
  info "✓ 实例已就绪（耗时 ${waited}s）"

  # 连不上的话再跑测例只会得到一堆无意义的 exit=1，所以在上面直接 die。

  # 跑 B1（已知通过，做回归）
  run_case ai_function ai_function_enhancement
  # 跑 B3 的五个用例
  run_case ai_column ai_column_ddl
  run_case ai_column ai_column_retry_strict
  run_case ai_column ai_column_t1_dml
  run_case ai_column ai_column_vector_index
  run_case ai_column ai_column_worker
}

run_case() {
  local suite="$1" case_name="$2"
  local dir="${SRC_DIR}/tools/deploy/mysql_test/test_suite/${suite}"

  if [ -z "${MYSQLTEST:-}" ]; then find_mysqltest || { printf '  ✗ 跳过 %s/%s（无 mysqltest）\n' "${suite}" "${case_name}"; return 1; }; fi
  local mt="${MYSQLTEST}"

  mkdir -p /tmp/mtr_var/tmp /tmp/mtr_var/log
  export LD_LIBRARY_PATH="${SRC_DIR}/deps/3rd/u01/obclient/lib:${LD_LIBRARY_PATH:-}"
  export OBSERVER_DIR=/tmp/sdb OBMYSQL_USR=root OBMYSQL_PWD= OBMYSQL_MS0=127.0.0.1 \
         OBMYSQL_PORT=2881 TENANT=mysql IS_BUSINESS=0

  "${mt}" --host=127.0.0.1 --port=2881 --user=root --password= --database=test \
    --tmpdir=/tmp/mtr_var/tmp --logdir=/tmp/mtr_var/log --silent --tail-lines=30 \
    --test-file="${dir}/t/${case_name}.test" \
    --result-file="${dir}/r/mysql/${case_name}.result" \
    --timer-file=/tmp/mtr_var/log/timer > "${LOG_DIR}/${suite}_${case_name}.log" 2>&1
  local rc=$?
  if [ ${rc} -eq 0 ]; then
    printf '  ✓ %s/%s PASS\n' "${suite}" "${case_name}"
  else
    printf '  ✗ %s/%s FAIL (exit=%s) → %s\n' "${suite}" "${case_name}" "${rc}" "${LOG_DIR}/${suite}_${case_name}.log"
    # 把失败点打出来，省得再去翻日志
    grep -nE "^---|^@@|^<|^>|error |Error " "${LOG_DIR}/${suite}_${case_name}.log" | head -30
  fi
  return ${rc}
}

case "${STAGE}" in
  check) stage_check ;;
  deps)  stage_deps ;;
  build) stage_build ;;
  test)  stage_test ;;
  all)   stage_deps; stage_build; stage_test ;;
  *)     echo "用法: bash $0 {check|deps|build|test|all}"; exit 1 ;;
esac
