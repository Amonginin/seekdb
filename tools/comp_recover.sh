#!/usr/bin/env bash
# 魔搭 DSW 实例重建脚本（seekdb 编译环境）
#
# 用法（在魔搭终端执行）：
#   bash /tmp/seekdb/tools/comp_recover.sh check    # 体检：NAS 备份、源码、工具链
#   bash /tmp/seekdb/tools/comp_recover.sh deps     # 装依赖（rpmfile 解 rpm + rust）
#   bash /tmp/seekdb/tools/comp_recover.sh build    # 编译 release
#   bash /tmp/seekdb/tools/comp_recover.sh bench L  # 干净实例 + FTS 完整基线（label=L）
#   bash /tmp/seekdb/tools/comp_recover.sh test     # 起 mock + 起库 + 跑 B1/B3 测例
#   bash /tmp/seekdb/tools/comp_recover.sh save     # 实例到期前：把环境搬到 NAS
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
# save_full 是三段式：先写 <out>.part，成功才 mv 成 <out> 并落 <out>.done 标记。
# 所以判断「有没有跑完」只看标记文件，不要去猜 tar 进程是否还在。
save_full_state() {
  local out="${NAS_DIR}/build_release.tar.gz"
  if [ -f "${out}.done" ] && [ -f "${out}" ]; then
    printf '  ✓ 完成：%s（%s）\n' "${out}" "$(du -h "${out}" 2>/dev/null | awk '{print $1}')"
  elif pgrep -f "tar -cf - -C ${SRC_DIR} build_release" >/dev/null 2>&1; then
    printf '  … 进行中：%s.part（已写 %s）\n' "${out}" \
      "$(du -h "${out}.part" 2>/dev/null | awk '{print $1}')"
  elif [ -f "${out}.part" ]; then
    printf '  ✗ 中断：%s.part 残留但没有 .done，重跑 save_full\n' "${out}"
  else
    printf '  - 未跑过\n'
  fi
}

stage_check() {
  log "体检"

  info "--- NAS 备份（${NAS_DIR}）---"
  if [ -d "${NAS_DIR}" ]; then
    ls -lh "${NAS_DIR}"/*.tgz "${NAS_DIR}"/*.tar.gz "${NAS_DIR}"/*.tar.zst 2>/dev/null \
      || info "(没有备份包)"
    info "save_full:"
    save_full_state
    df -h "${NAS_DIR}" 2>/dev/null | tail -1
  else
    info "${NAS_DIR} 不存在（不是魔搭 DSW？）"
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
  #
  # rust-toolchain.toml 钉了 5 个 target（含两个 iOS）。rustup 装工具链时会
  # 读那个文件自动补装，iOS 的 std 组件有几百 MB，在 Linux 上下载极慢——
  # 这一步曾经把实例拖到超时。
  #
  # 但本机只需要 x86_64-unknown-linux-gnu：cmake/Rust.cmake 只在 APPLE 和
  # ANDROID 下才切 target，Linux 上就是宿主机三元组。所以只装这一个，
  # 并用 RUSTUP_TOOLCHAIN 绕开工具链文件的自动补装。
  export PATH="$HOME/.cargo/bin:${PATH}"
  if ! command -v cargo >/dev/null 2>&1 || ! cargo --version 2>/dev/null | grep -q "1\.98\.1"; then
    info "装 Rust 1.98.1（只装 Linux target，iOS/Android 交叉编译用不到）"
    export RUSTUP_DIST_SERVER=https://rsproxy.cn
    export RUSTUP_UPDATE_ROOT=https://rsproxy.cn/rustup
    curl --proto '=https' --tlsv1.2 -sSf https://rsproxy.cn/rustup-init.sh \
      | sh -s -- -y --default-toolchain none > "${LOG_DIR}/rustup_init.log" 2>&1
    [ -f "$HOME/.cargo/env" ] && . "$HOME/.cargo/env"
    export PATH="$HOME/.cargo/bin:${PATH}"
    # --no-self-update：工具链自更新会卡在网络请求上。
    #
    # 不带 --target 参数：rustup 会向上找 rust-toolchain.toml 并补装它列的
    # 全部 5 个 target，iOS 那两个在这里纯属浪费时间和带宽。但只要当前目录
    # 链路上有这个文件，rustup 就会照办——所以在 /tmp 下装，那里没有工具链
    # 文件。装完再显式补上 Linux target。
    ( cd /tmp && rustup toolchain install 1.98.1 --profile minimal --component clippy \
        --no-self-update ) 2>&1 | tail -5
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

  # 1f. 只要求 Linux target。少了它编译会失败，多装只是浪费时间。
  if command -v rustup >/dev/null 2>&1; then
    if ! rustup target list --installed --toolchain 1.98.1 2>/dev/null | grep -qx "x86_64-unknown-linux-gnu"; then
      info "补装 x86_64-unknown-linux-gnu"
      export RUSTUP_DIST_SERVER=https://rsproxy.cn
      export RUSTUP_UPDATE_ROOT=https://rsproxy.cn/rustup
      rustup target add --toolchain 1.98.1 x86_64-unknown-linux-gnu 2>&1 | tail -3 \
        || die "rust target 补装失败"
    fi
    rustup default 1.98.1 >/dev/null 2>&1
  fi
  info "✓ 依赖就绪"
}

# ---------------------------------------------------------------- build
stage_build() {
  log "阶段 2/3：编译（release）"
  cd "${SRC_DIR}" || die "找不到 ${SRC_DIR}"

  # 拉最新代码。deps/ 是解包出来的依赖，不能跟着覆盖掉。
  # merge --ff-only 而不是 reset --hard：只有能快进时才移动 HEAD，
  # 本地有未提交改动时它会拒绝而不是把改动丢掉。
  sync_source || die "同步代码失败，先解决网络或本地改动的问题"

  # 先检查 bench 脚本。`local` 用在函数之外语法合法、运行时才报错，
  # 而报错发生在跑完整轮 bench 之后 —— 一轮编译加一轮 bench 的时间就没了。
  # 这个检查只要几秒。
  if [ -f tools/benchmark/check_bench_scripts.py ]; then
    if ! python3 tools/benchmark/check_bench_scripts.py; then
      die "bench 脚本检查不通过，先修脚本再编译"
    fi
  fi

  pkill -f "seekdb --base-dir" 2>/dev/null; sleep 2

  # cargo 会向上找 rust/rust-toolchain.toml，发现缺 target 就去补装——
  # iOS 的 std 有几百 MB，Linux 上下载极慢，这会拖垮整个编译。
  # 设 RUSTUP_TOOLCHAIN 让它忽略工具链文件，只用已装好的 1.98.1。
  export RUSTUP_TOOLCHAIN=1.98.1
  export PATH="$HOME/.cargo/bin:${PATH}"

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
# 回收所有 mock 进程。残留的 mock 会占住端口，让新 mock 绑不上而请求打到旧
# 进程上，表现为调用超时或"没有收到任何请求"。
kill_mocks() {
  pkill -9 -f ai_column_mock.py 2>/dev/null
  pkill -9 -f ai_function_mock.py 2>/dev/null
  for p in 8000 19120 19121 19122 19123 19124; do
    local owners
    owners=$(ss -lntp 2>/dev/null | grep ":${p} " | grep -o "pid=[0-9]*" | cut -d= -f2 | sort -u)
    for pid in ${owners}; do
      info "端口 ${p} 被 pid ${pid} 占用，强制结束"
      kill -9 "${pid}" 2>/dev/null
    done
  done
  sleep 2
}

# 干净地起一个实例。用例之间必须隔离：某个用例超时后留下的实例状态会
# 污染后面的用例，所以每次跑用例前都重新起。
start_instance() {
  # 上一轮的服务进程必须先停掉：下面会删掉它正在用的数据目录，
  # 留着它还会占住 2881，新实例绑不上端口，测试就会在握手阶段断连。
  pkill -9 -f "observer/seekdb" 2>/dev/null
  pkill -9 -f "seekdb --base-dir" 2>/dev/null
  sleep 2
  # 等到端口真正空出来。占用的进程可能不是上面两个模式匹配到的，
  # 所以这里以端口状态为准，并把占用者打出来。
  local port_wait=0
  while [ ${port_wait} -lt 15 ]; do
    if ! (exec 3<>/dev/tcp/127.0.0.1/2881) 2>/dev/null; then
      break
    fi
    sleep 1
    port_wait=$((port_wait + 1))
  done
  if (exec 3<>/dev/tcp/127.0.0.1/2881) 2>/dev/null; then
    info "端口 2881 仍被占用，占用者："
    ss -lntp 2>/dev/null | grep 2881
    ps -ef | grep -i seekdb | grep -v grep
    die "端口 2881 被占用，无法启动实例"
  fi
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
  if ! kill -0 ${server_pid} 2>/dev/null; then
    # 端口是通的但不是我们启动的进程，说明有残留实例占着 2881。
    info "端口 2881 被其他进程占用，我们启动的实例已退出。server.log 末尾："
    tail -40 /tmp/sdb/server.log
    die "实例启动失败（端口被占用）"
  fi
  # 端口监听发生在初始化早期，实例可能之后才崩。等几秒再确认一次，
  # 否则测试会在握手阶段每次等两分钟超时。
  sleep 5
  if ! kill -0 ${server_pid} 2>/dev/null; then
    info "实例在端口就绪后退出，server.log 末尾："
    tail -40 /tmp/sdb/server.log
    info "崩溃信息（如果有）："
    ls -t /tmp/sdb/log/*.log 2>/dev/null | head -1 | xargs -r tail -20
    die "实例启动后崩溃"
  fi
  info "✓ 实例已就绪（耗时 ${waited}s）"

  # 全新 bootstrap 的实例没有 test 库，而 mysqltest 用 --database=test 连接，
  # 缺库会让每个用例都直接在建立连接时失败。
  local mysql_cli=""
  local c
  for c in "${SRC_DIR}/deps/3rd/u01/obclient/bin/obclient" \
           "${SRC_DIR}/deps/3rd/u01/obclient/bin/mysql"; do
    if [ -x "${c}" ]; then
      mysql_cli="${c}"
      break
    fi
  done
  if [ -n "${mysql_cli}" ]; then
    export LD_LIBRARY_PATH="${SRC_DIR}/deps/3rd/u01/obclient/lib:${LD_LIBRARY_PATH:-}"
    if "${mysql_cli}" -h127.0.0.1 -P2881 -uroot --protocol=tcp \
         -e "create database if not exists test;" >/dev/null 2>&1; then
      info "✓ 已确保 test 库存在"
    else
      info "! 建 test 库失败，继续尝试"
    fi
    # 官方用例里写死了 connect ...,admin,$OBMYSQL_PWD,...（如 file_sql 的两个
    # include），而 bootstrap 只建了 root。这里补一个空密码的 admin。
    if ! "${mysql_cli}" -h127.0.0.1 -P2881 -uroot --protocol=tcp \
           -e "create user admin identified by '';" >/dev/null 2>&1; then
      # 已存在但密码未知时，改一次空密码
      "${mysql_cli}" -h127.0.0.1 -P2881 -uroot --protocol=tcp \
        -e "alter user admin identified by '';" >/dev/null 2>&1
    fi
    if "${mysql_cli}" -h127.0.0.1 -P2881 -uroot --protocol=tcp \
         -e "grant all privileges on *.* to admin with grant option;" >/dev/null 2>&1; then
      info "✓ admin 用户可用（空密码，已授权）"
    else
      info "! 授权 admin 失败"
    fi
  else
    info "! 未找到 obclient/mysql 客户端，跳过建库与 admin 用户"
  fi
}

# 官方用例（如 file_sql 的两个 include）用 ssh/scp 把数据摆到「本机」：
#   ssh $FILE_SQL_HOST 'cmd' 与 scp data/* $FILE_SQL_HOST:dir
# 容器里通常没有 sshd，这里放两个只在本机执行的 shim 到 PATH 最前面，
# 不修改任何 test_suite 文件，语义等同在同一台机器上拷贝。
install_local_ssh_shims() {
  local d="${LOG_DIR}/shim"
  mkdir -p "${d}"
  cat > "${d}/ssh" <<'SH'
#!/usr/bin/env bash
# 忽略 host 参数，直接在本地执行剩余命令
shift || true
exec bash -c "$*"
SH
  cat > "${d}/scp" <<'SH'
#!/usr/bin/env bash
# 把 <host>:<path> 还原成本地路径，其余按普通 cp 处理
srcs=()
dest=""
for a in "$@"; do
  case "$a" in
    -*) ;;
    *:*) dest="${a#*:}" ;;
    *) srcs+=("$a") ;;
  esac
done
if [ -z "${dest}" ] || [ ${#srcs[@]} -eq 0 ]; then
  exit 1
fi
cp -f "${srcs[@]}" "${dest}/"
SH
  chmod +x "${d}/ssh" "${d}/scp"
  export PATH="${d}:${PATH}"
  info "✓ 已启用本地 ssh/scp shim（${d}）"
}

# 只起一个干净的实例，不跑任何用例。压测（tools/benchmark）和手工 SQL 前用它。
stage_start() {
  log "起一个干净的实例"
  start_instance
  info "✓ 可用：127.0.0.1:2881，root 空密码，数据目录 /tmp/sdb"
}

# 跑 B4 的完整基线（build / tokenize / query 三类指标）。
#
# 必须走 start_instance：它会先 rm -rf /tmp/sdb。复用旧的 base-dir 是踩过的坑——
# 上一轮遗留的索引表和后台合并会把机器打满，raw_load_sec 从 0.3s 劣化到 2.7s，
# build 三项全部失真。所以这里不接受「已经有实例在跑」这种省事做法。
stage_bench() {
  local label="${1:-bench}"
  log "跑 FTS 完整基线（label=${label}）"
  start_instance

  local cli=""
  local c
  for c in "${SRC_DIR}/deps/3rd/u01/obclient/bin/obclient" \
           "${SRC_DIR}/deps/3rd/u01/obclient/bin/mysql"; do
    if [ -x "${c}" ]; then
      cli="${c}"
      break
    fi
  done
  if [ -z "${cli}" ]; then
    cli="$(command -v obclient 2>/dev/null || true)"
  fi
  [ -n "${cli}" ] || die "找不到 obclient"
  export LD_LIBRARY_PATH="${SRC_DIR}/deps/3rd/u01/obclient/lib:${LD_LIBRARY_PATH:-}"

  local out="${LOG_DIR}/fts_${label}.txt"
  ( cd "${SRC_DIR}" && \
    MYSQL="${cli} -h127.0.0.1 -P2881 -uroot -N -s --protocol=tcp --default-character-set=utf8mb4" \
    MYSQL_VERBOSE="${cli} -h127.0.0.1 -P2881 -uroot --protocol=tcp --default-character-set=utf8mb4" \
    LABEL="${label}" OUTPUT="${out}" BUILD_REPEATS="${BUILD_REPEATS:-1}" \
    PROBE_THREADS="${PROBE_THREADS:-0}" \
    bash tools/benchmark/fts_large_bench.sh ) || die "bench 失败，看 ${out}"

  log "关键指标"
  grep -E "select1_avg_ms:|raw_load_sec:|build_.*_sec:|tokenize_.*_avg_ms:|query_.*(avg_ms|hits):" \
    "${out}" | tail -16 | sed 's/^/  /'

  # A bench run that lost the indexes reports zero for every hit count, and the
  # timings that come back are the cost of the error path. Say so instead of
  # letting a broken run look like a good one.
  local bad_hits
  bad_hits=$(grep -E "^(query_cn_hits|query_beng_hits|query_mixed_hits|query_limit_hits):" "${out}" \
    | grep -vE ":[[:space:]]*[1-9][0-9]*$" | head -4)
  if [ -n "${bad_hits}" ]; then
    log "!! 命中行数为 0 或缺失，本轮查询结果无效："
    printf '%s\n' "${bad_hits}" | sed 's/^/     /'
  fi
  local expected
  expected=$(grep -E "^(query_cn_hits|query_beng_hits|query_mixed_hits|query_limit_hits):" "${out}" | tr -s ' ' | cut -d' ' -f2 | tr '\n' ' ')
  log "命中行数: ${expected:-无}" 

  # raw_load 是「这一轮可不可信」的哨兵：它在所有 ALTER 之前，不受 build 代码影响。
  local rl
  rl=$(grep -m1 "raw_load_sec:" "${out}" | awk '{print $2}')
  awk -v v="${rl:-0}" 'BEGIN {
    if (v + 0 > 1.0) printf "  ! raw_load_sec=%ss 偏高，本轮数据很可能被后台合并干扰\n", v;
    else printf "  ✓ raw_load_sec=%ss 正常，本轮可作 A/B\n", v }'

  log "DDL 切片数分布（出现 >1 才说明并行建索引生效）"
  grep -rh "dag start process" /tmp/sdb/log/ 2>/dev/null \
    | grep -o "total_slice_count:[0-9]*" | sort | uniq -c | sed 's/^/  /'

  log "倒排链缓存"
  info "cached: $(grep -rh "fts inverted chain cached" /tmp/sdb/log/ 2>/dev/null | wc -l)"
  info "hit:    $(grep -rh "fts inverted chain hit" /tmp/sdb/log/ 2>/dev/null | tail -1 | sed 's/.*] //')"

  info "✓ 报告：${out}"
}

# 采样 build 阶段的热点：干净实例 → 灌 20000 篇 → perf/gdb 采样三次 ALTER → 打热点报告。
# 不跑分词/查询压测，一轮几分钟，用来回答「build 的 4.2s/索引 花在哪个函数」。
stage_prof() {
  local label="${1:-prof}"
  log "采样 build 热点（label=${label}）"
  start_instance

  local cli="" c
  for c in "${SRC_DIR}/deps/3rd/u01/obclient/bin/obclient" \
           "${SRC_DIR}/deps/3rd/u01/obclient/bin/mysql"; do
    if [ -x "${c}" ]; then cli="${c}"; break; fi
  done
  if [ -z "${cli}" ]; then
    cli="$(command -v obclient 2>/dev/null || true)"
  fi
  [ -n "${cli}" ] || die "找不到 obclient"
  export LD_LIBRARY_PATH="${SRC_DIR}/deps/3rd/u01/obclient/lib:${LD_LIBRARY_PATH:-}"

  local out="${LOG_DIR}/fts_${label}.txt"
  ( cd "${SRC_DIR}" && \
    MYSQL="${cli} -h127.0.0.1 -P2881 -uroot -N -s --protocol=tcp --default-character-set=utf8mb4" \
    MYSQL_VERBOSE="${cli} -h127.0.0.1 -P2881 -uroot --protocol=tcp --default-character-set=utf8mb4" \
    LABEL="${label}" OUTPUT="${out}" PROFILE_BUILD=1 \
    bash tools/benchmark/fts_large_bench.sh ) || die "采样失败，看 ${out}"

  if [ -f "${out}" ]; then
    log "构建耗时（采样轮；perf 在计时窗口外启停，数字仍可参考）"
    grep -E "^build_.*_sec:" "${out}" | sed 's/^/  /'
    info "✓ 完整报告：${out}"
  else
    info "! 报告没落盘，热点只在上面终端输出里"
  fi
}

stage_test() {
  log "阶段 3/3：跑测例"
  kill_mocks
  # 工作目录必须是 tools/deploy：测例里的 --exec 写的是 mysql_test/test_suite/...
  cd "${SRC_DIR}/tools/deploy" || die "找不到 tools/deploy"
  install_local_ssh_shims
  python3 mysql_test/test_suite/ai_column/include/ai_column_mock.py start 19120 > "${LOG_DIR}/mock.log" 2>&1
  python3 mysql_test/test_suite/ai_function/include/ai_function_mock.py start 8000 >> "${LOG_DIR}/mock.log" 2>&1
  sleep 2
  info "✓ mock 已启动（ai_column:19120, ai_function:8000）"

  start_instance

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

# 只跑指定用例，每个用例前重启实例。
# 用法：bash tools/comp_recover.sh case ai_column/ai_column_t1_dml ai_column/ai_column_worker
stage_case() {
  shift
  log "阶段 4：跑指定用例"
  if [ $# -eq 0 ]; then
    die "用法: bash $0 case <套件/用例> [更多...]，例如 case ai_column/ai_column_worker"
  fi

  kill_mocks
  cd "${SRC_DIR}/tools/deploy" || die "找不到 tools/deploy"
  install_local_ssh_shims

  for spec in "$@"; do
    local suite="${spec%%/*}"
    local case_name="${spec##*/}"
    if [ "${suite}" = "${spec}" ]; then
      suite="ai_column"
    fi
    # 每个用例都从干净实例开始：上一个用例超时后留下的状态会污染下一个。
    start_instance
    run_case "${suite}" "${case_name}"
  done
}

# ---------------------------------------------------------------- save
# 实例到期前把可复用的东西搬到 NAS。NAS 上的文件不受实例回收影响。
#
# 代码是第一位：必须先推到 GitHub，NAS 备份只是兜底。顺序不能反。
NAS_DIR="${NAS_DIR:-/mnt/workspace}"

# 有 pigz 就用它，并行压缩快很多；没有就用 gzip。
# 不走 tar -I：它和 -z 同时给会冲突。
NCPU="$(nproc 2>/dev/null || echo 4)"
[ "${NCPU}" -ge 1 ] 2>/dev/null || NCPU=4

HAVE_PIGZ=0
command -v pigz >/dev/null 2>&1 && HAVE_PIGZ=1
HAVE_ZSTD=0
command -v zstd >/dev/null 2>&1 && HAVE_ZSTD=1

# 压缩方式。
#   portable —— 产物是标准 gzip，任何机器的 gzip 都能解（pigz 的输出也是 gzip 格式）
#   fast    —— 产物是 zstd，压缩率更高、NAS 写入更快，但解压端需要 zstd 命令
PACK_MODE="${PACK_MODE:-portable}"

# pack <输出基名> <tar 参数...>
# 写出的文件路径放在全局 PACK_OUT 里，扩展名按实际用的压缩器定。
PACK_OUT=""
pack() {
  local base="$1"; shift
  local out tmpd
  if [ "${PACK_MODE}" = "fast" ] && [ "${HAVE_ZSTD}" = "1" ]; then
    out="${base}.tar.zst"
    tar -cf - "$@" | zstd -T"${NCPU}" -3 -q -o "${out}" || return 1
    PACK_OUT="${out}"
    return 0
  fi
  out="${base}.tar.gz"
  if [ "${HAVE_PIGZ}" = "1" ]; then
    tar -cf - "$@" | pigz -6 > "${out}" || return 1
  else
    # 兜底：把 tar 流切成块，每块独立 gzip，再按序拼起来。
    # gzip 的多个成员可以串联，标准 gzip -d 一次就能解开，所以产物
    # 依然是普通 gzip 文件，解压端不需要装任何东西。
    tmpd="$(mktemp -d /tmp/pack.XXXXXX)" || return 1
    if ! tar -cf - "$@" | split -b 512M -a 3 - "${tmpd}/part-"; then
      rm -rf "${tmpd}"; return 1
    fi
    if ! find "${tmpd}" -name 'part-*' ! -name '*.gz' -print0 \
         | xargs -0 -P "${NCPU}" -I{} sh -c 'gzip -6 -c "$1" > "$1.gz"' _ {}; then
      rm -rf "${tmpd}"; return 1
    fi
    # part-000 … part-063，名字定长，字典序就是分块顺序
    if ! cat "${tmpd}"/part-*.gz > "${out}"; then
      rm -rf "${tmpd}"; return 1
    fi
    rm -rf "${tmpd}"
  fi
  PACK_OUT="${out}"
  return 0
}

# unpack <压缩包> <目标目录>
unpack() {
  local f="$1" dest="$2"
  mkdir -p "${dest}"
  case "${f}" in
    *.tar.zst)
      if [ "${HAVE_ZSTD}" != "1" ]; then
        info "! 没有 zstd 命令，无法解开 ${f}"
        return 1
      fi
      zstd -d -T"${NCPU}" -q -c "${f}" | tar -xf - -C "${dest}"
      ;;
    *.tar.gz|*.tgz)
      # .tgz 是旧脚本存的名字，格式和 .tar.gz 完全一样
      if [ "${HAVE_PIGZ}" = "1" ]; then
        pigz -dc "${f}" | tar -xf - -C "${dest}"
      else
        gzip -dc "${f}" | tar -xf - -C "${dest}"
      fi
      ;;
    *) return 1 ;;
  esac
}

stage_save() {
  log "保存环境到 ${NAS_DIR}"

  if [ ! -d "${NAS_DIR}" ]; then
    die "${NAS_DIR} 不存在。这不是魔搭 DSW 实例，保存没有意义。"
  fi
  mkdir -p "${NAS_DIR}"

  # 1) 源码包（含 .git 历史）。排除编译产物和依赖，它们单独存。
  #    代码不需要另外推送：包里带着 .git，恢复后历史全在。
  info "打包源码（不含 build_release / deps）"
  if [ -d "${SRC_DIR}" ]; then
    pack "${NAS_DIR}/seekdb_src" \
      -C /tmp --exclude='seekdb/build_release' --exclude='seekdb/deps/3rd' seekdb \
      || die "源码打包失败"
    info "✓ ${PACK_OUT}"
  fi

  # 2) 依赖（bison / clang / cmake / obclient）。省掉 10 分钟的 rpm 解包。
  if [ -d "${SRC_DIR}/deps/3rd" ]; then
    info "打包依赖 deps/3rd（耗时几分钟）"
    pack "${NAS_DIR}/deps3rd" -C "${SRC_DIR}/deps" 3rd || die "依赖打包失败"
    info "✓ ${PACK_OUT}"
  fi

  # 3) Rust 工具链。5 个 target 重新下载很慢。
  if [ -d "$HOME/.cargo" ]; then
    info "打包 Rust 工具链"
    pack "${NAS_DIR}/cargo" -C "$HOME" .cargo || die "Rust 打包失败"
    info "✓ ${PACK_OUT}"
  fi

  # 4) 测试日志。判题失败时的现场证据，比二进制更难重造。
  if [ -d /tmp/recover ]; then
    pack "${NAS_DIR}/recover_logs" -C /tmp recover && info "✓ ${PACK_OUT}"
  fi

  info "--- NAS 上的备份 ---"
  ls -lh "${NAS_DIR}"/*.tar.* "${NAS_DIR}"/*.txt 2>/dev/null
  df -h "${NAS_DIR}" | tail -1
  info "✓ 保存完成。新实例上执行：bash tools/comp_recover.sh restore"
}

# 完整编译树很大（十几 GB），压缩要十几分钟，放后台跑。
# 留着它才能在新实例上做增量编译，否则每次都要全量重编。
stage_save_full() {
  log "保存完整编译树（增量编译用）"
  if [ ! -d "${NAS_DIR}" ]; then
    die "${NAS_DIR} 不存在"
  fi
  if [ ! -d "${SRC_DIR}/build_release" ]; then
    die "${SRC_DIR}/build_release 不存在，先编译"
  fi
  local out="${NAS_DIR}/build_release.tar.gz"
  info "后台打包 ${SRC_DIR}/build_release → ${out}"
  info "预计十几分钟。用 tail -f ${LOG_DIR}/save_full.log 跟踪"

  # 后台子 shell 里拿不到 pack 函数，所以把压缩命令直接展开。
  # 有 pigz 就走 pigz（并行），没有就退回 tar 自带的 gzip（单线程，慢一点）。
  local comp
  if [ "${HAVE_PIGZ}" = "1" ]; then
    comp="pigz -1"
  else
    comp="gzip -1"
    info "! 没有 pigz，将用单线程 gzip，压缩时间会长很多"
  fi
  nohup bash -c "tar -cf - -C '${SRC_DIR}' build_release | ${comp} > '${out}.part' \
    && mv '${out}.part' '${out}' && echo DONE > '${out}.done'" \
    > "${LOG_DIR}/save_full.log" 2>&1 &
  info "✓ 后台任务 pid=$!（${NCPU} 核）"
  info "查进度：bash $0 check   （看 build_release.tar.gz 状态）"
  info "完成标志：${out}.done 出现；中途按 Ctrl-C 不会产生 .done，重跑即可"
}

# ---------------------------------------------------------------- restore
# 找一个备份包。NAS 上同时存在多代命名：
#   <base>.tar.zst / <base>.tar.gz    新格式
#   <base>.tgz                       旧格式
#   <base>_YYYYMMDD.tgz              带日期后缀的旧格式
# 取 mtime 最新的那个，不按名字偏好——日期后缀的那份往往更新。
find_backup() {
  local base="$1"
  local newest
  newest=$(ls -1t "${NAS_DIR}/${base}.tar.zst" "${NAS_DIR}/${base}.tar.gz" \
                   "${NAS_DIR}/${base}.tgz" "${NAS_DIR}/${base}"_*.tgz \
                   "${NAS_DIR}/${base}"_*.tar.gz "${NAS_DIR}/${base}"_*.tar.zst \
                   2>/dev/null | head -1)
  if [ -n "${newest}" ] && [ -f "${newest}" ]; then
    printf '%s' "${newest}"
    return 0
  fi
  return 1
}

# 把源码同步到远端最新提交。
#
# NAS 上的源码包可能是在别的实例上打的，里面记的 remote 是当时的代理地址，
# 而那台实例可能已经没了。所以先把 remote 归一化到已知的可用地址，
# 再 fetch + 快进。fetch 走的是 GitHub，增量只有几十 KB，比 clone 快得多。
sync_source() {
  local repo_url="https://gh-proxy.com/https://github.com/Amonginin/seekdb.git"
  [ -d "${SRC_DIR}/.git" ] || return 0

  # 归一化 remote：包里的地址可能失效或指向别的仓库
  local cur
  cur=$( cd "${SRC_DIR}" && git remote get-url origin 2>/dev/null )
  if [ "${cur}" != "${repo_url}" ]; then
    info "  修正 origin：${cur:-（空）} → ${repo_url}"
    ( cd "${SRC_DIR}" && git remote set-url origin "${repo_url}" )
  fi

  info "  fetch 增量（只拉新提交，比 clone 快得多）"
  if ! ( cd "${SRC_DIR}" && git fetch origin 2026-competition 2>&1 | tail -2 ); then
    info "  fetch 失败，可能 GitHub 暂时不通"
    return 1
  fi

  # 快进。快进不了说明本地有分叉或未提交改动，不能强行覆盖。
  if ( cd "${SRC_DIR}" && git merge --ff-only FETCH_HEAD ) 2>/dev/null; then
    info "  ✓ 已同步到最新"
  else
    info "  ! 不能快进（本地有分叉或未提交改动），保持现状："
    ( cd "${SRC_DIR}" && git status --short | head -5 | sed 's/^/    /' )
  fi
  ( cd "${SRC_DIR}" && git log --oneline -1 | sed 's/^/  当前 /' )
}

stage_restore() {
  log "从 NAS 恢复环境"

  if [ ! -d "${NAS_DIR}" ]; then
    die "${NAS_DIR} 不存在"
  fi

  # 1) 源码。NAS 上的源码包优先：它是完整 git 仓库，解包后 fetch 即可拿到
  #    最新提交，比 clone 少传 100 多 MB，也不依赖 GitHub 能不能连上。
  #    clone 只作为兜底（NAS 上没有源码包时）。
  local src_pkg
  if src_pkg=$(find_backup seekdb_src); then
    info "解包源码：${src_pkg}"
    if [ -d "${SRC_DIR}" ]; then
      mv "${SRC_DIR}" "${SRC_DIR}.old.$(date +%s)"
    fi
    unpack "${src_pkg}" /tmp || die "源码解包失败"
    info "✓ 源码已恢复"
    sync_source
  elif [ -d "${SRC_DIR}/.git" ]; then
    info "没有源码包，但 ${SRC_DIR} 已有仓库，直接快进"
    sync_source
  else
    # 2026-11-09 实例到期时源码包没打成，就是走的这条路。
    info "没有源码包，改用 clone（代码在 GitHub 上，不会丢）"
    git clone -b 2026-competition \
      https://gh-proxy.com/https://github.com/Amonginin/seekdb.git "${SRC_DIR}" \
      || die "clone 失败"
  fi

  # 2) 依赖。
  local deps_pkg
  if deps_pkg=$(find_backup deps3rd); then
    info "解包依赖：${deps_pkg}"
    mkdir -p "${SRC_DIR}/deps"
    unpack "${deps_pkg}" "${SRC_DIR}/deps" || die "依赖解包失败"
    # dep_create.sh 会 rm -rf deps/3rd 再重下，标记能让它提前退出
    local dep_md5
    dep_md5=$(md5sum "${SRC_DIR}/deps/init/oceanbase.el9.x86_64.deps" | cut -d' ' -f1)
    touch "${SRC_DIR}/deps/3rd/DONE" "${SRC_DIR}/deps/3rd/${dep_md5}"
    info "✓ 依赖已恢复（已打 DONE 标记）"
  else
    info "! 没有依赖包，需要跑：bash $0 deps"
  fi

  # 3) Rust。~/.cargo 不在 NAS 上，只有我们自己存过才有；否则必须重装。
  local cargo_pkg
  if cargo_pkg=$(find_backup cargo); then
    info "解包 Rust 工具链：${cargo_pkg}"
    unpack "${cargo_pkg}" "$HOME" || info "  Rust 解包失败，重装即可"
    [ -f "$HOME/.cargo/env" ] && . "$HOME/.cargo/env"
    export PATH="$HOME/.cargo/bin:${PATH}"
    info "✓ Rust 已恢复"
  else
    info "! 没有 Rust 包，跑：bash $0 deps（会装 Rust 1.98.1）"
  fi

  # 4) 完整编译树（可选，能把重编从 60 分钟压到 10 分钟）。
  #    注意：编译树里的绝对路径是 /tmp/seekdb，换目录解包增量就失效。
  local tree_pkg
  if tree_pkg=$(find_backup build_release); then
    info "解包编译树（几分钟）：${tree_pkg}"
    if unpack "${tree_pkg}" "${SRC_DIR}"; then
      info "✓ 编译树已恢复"
      # 编译树比当前代码旧是正常的，但 CMake 缓存里的路径必须对得上，
      # 否则会当成全新工程重编。检查一下缓存里的路径。
      if [ -f "${SRC_DIR}/build_release/CMakeCache.txt" ]; then
        if grep -q "${SRC_DIR}" "${SRC_DIR}/build_release/CMakeCache.txt" 2>/dev/null; then
          info "  ✓ CMake 缓存路径匹配，增量编译可用"
        else
          info "  ! CMake 缓存里的路径不是 ${SRC_DIR}，可能变全量编译"
        fi
      fi
    else
      info "  解包失败，可重编"
    fi
  elif [ -f "${NAS_DIR}/build_release.tgz.part" ]; then
    info "! build_release.tgz.part 存在但没有完整的包，说明上次没存完，忽略"
  fi

  # 5) 对账：恢复出来的代码是哪个提交。
  if [ -d "${SRC_DIR}/.git" ]; then
    info "恢复出来的提交："
    ( cd "${SRC_DIR}" && git log --oneline -1 | sed 's/^/  /' )
  fi

  info "--- 体检 ---"
  stage_check
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

  # 单个用例最多 240 秒，卡住就杀掉，避免整个阶段无限等待。
  timeout 240 "${mt}" --host=127.0.0.1 --port=2881 --user=root --password= --database=test \
    --tmpdir=/tmp/mtr_var/tmp --logdir=/tmp/mtr_var/log --silent --tail-lines=30 \
    --test-file="${dir}/t/${case_name}.test" \
    --result-file="${dir}/r/mysql/${case_name}.result" \
    --timer-file=/tmp/mtr_var/log/timer > "${LOG_DIR}/${suite}_${case_name}.log" 2>&1
  local rc=$?
  if [ ${rc} -eq 124 ]; then
    printf '  ✗ %s/%s TIMEOUT（超过 240s，已杀掉）\n' "${suite}" "${case_name}"
    printf '    最后执行的语句：\n'
    grep -nE "^(create|insert|select|alter|drop|update|delete|call|commit|begin|start)" \
      "${LOG_DIR}/${suite}_${case_name}.log" | tail -5 | sed 's/^/    /'
    return 1
  fi
  if [ ${rc} -eq 0 ]; then
    printf '  ✓ %s/%s PASS\n' "${suite}" "${case_name}"
  else
    printf '  ✗ %s/%s FAIL (exit=%s) → %s\n' "${suite}" "${case_name}" "${rc}" "${LOG_DIR}/${suite}_${case_name}.log"
    # mysqltest 把"连不上"这类错误写在最前面，diff 反而不一定有，所以两段都打。
    printf '    --- log head ---\n'
    head -15 "${LOG_DIR}/${suite}_${case_name}.log" | sed 's/^/    /'
    printf '    --- diff ---\n'
    grep -nE "^---|^@@|^<|^>|^[0-9]+:" "${LOG_DIR}/${suite}_${case_name}.log" | head -20 | sed 's/^/    /'
    # 断言失败发生在哪一条语句，只有尾部看得到。
    printf '    --- log tail ---\n'
    tail -30 "${LOG_DIR}/${suite}_${case_name}.log" | sed 's/^/    /'
  fi
  return ${rc}
}

case "${STAGE}" in
  check) stage_check ;;
  deps)  stage_deps ;;
  build) stage_build ;;
  test)  stage_test ;;
  start) stage_start ;;
  bench) stage_bench "${2:-bench}" ;;
  prof)  stage_prof "${2:-prof}" ;;
  case)  stage_case "$@" ;;
  save)  stage_save ;;
  save_full) stage_save_full ;;
  restore) stage_restore ;;
  all)   stage_deps; stage_build; stage_test ;;
  *)     echo "用法: bash $0 {check|deps|build|test|start|bench|prof|save|save_full|restore|all}"; echo "      bash $0 bench [label]     # 干净实例 + FTS 完整基线"; echo "      bash $0 prof  [label]     # 只采样 build 热点（perf/gdb），不跑分词/查询"; echo "      bash $0 case <套件/用例> [更多...]"; exit 1 ;;
esac
