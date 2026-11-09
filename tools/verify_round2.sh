#!/usr/bin/env bash
# 第二轮验证：enable_file_sql 变量注册（题2）+ B3 WITH 选项（题3）
#
# 用法（在魔搭终端）：
#   bash /tmp/seekdb/tools/verify_round2.sh sync     # 同步到 s2026 最新并检查关键点
#   bash /tmp/seekdb/tools/verify_round2.sh build    # 编译
#   bash /tmp/seekdb/tools/verify_round2.sh check    # 编译后自查（不重编译）
#   bash /tmp/seekdb/tools/verify_round2.sh mysqltest # 跑官方测例
#
# 设计：分阶段、可重复执行、每步都打明确结论。

set -uo pipefail

SRC_DIR="${SRC_DIR:-/tmp/seekdb}"
LOG_DIR="${LOG_DIR:-/tmp/verify}"
STAGE="${1:-help}"
mkdir -p "${LOG_DIR}"

log()  { printf '\n=== %s ===\n' "$*"; }
info() { printf '  %s\n' "$*"; }
ok()   { printf '  ✓ %s\n' "$*"; }
bad()  { printf '  ✗ %s\n' "$*"; }
die()  { printf '  ✗ %s\n' "$*" >&2; exit 1; }

export PATH="${SRC_DIR}/deps/3rd/usr/local/oceanbase/devtools/bin:${PATH}"

# ---------------------------------------------------------------- sync
stage_sync() {
  log "同步到 s2026 最新"

  cd "${SRC_DIR}" || die "源码不在 ${SRC_DIR}"

  git fetch s2026 2026-competition 2>&1 | tail -3 || die "fetch 失败"
  git checkout FETCH_HEAD -- src/ tools/ deps/ 2>&1 | tail -3
  git log --oneline -1

  log "关键点自查"

  # 1. enable_file_sql 是否在 json 里
  if grep -q '"enable_file_sql"' src/share/system_variable/ob_system_variable_init.json; then
    ok "json 里有 enable_file_sql"
  else
    bad "json 里没有 enable_file_sql —— 变量没注册"
  fi

  # 2. var_amount 必须是 720（这是 1193 的真根因）
  local va
  va=$(grep -oP 'var_amount = \K[0-9]+' src/share/system_variable/ob_system_variable_init.cpp | head -1)
  if [ "${va}" = "720" ]; then
    ok "var_amount = 720"
  else
    bad "var_amount = ${va}（应为 720，名字查找扫不到第 720 个变量 → 报 1193）"
  fi

  # 3. OB_SYS_VARS_COUNT 必须是 621
  local cnt
  cnt=$(grep -oP 'OB_SYS_VARS_COUNT = \K[0-9]+' src/share/system_variable/ob_sys_var_meta.h | head -1)
  if [ "${cnt}" = "621" ]; then
    ok "OB_SYS_VARS_COUNT = 621"
  else
    bad "OB_SYS_VARS_COUNT = ${cnt}（应为 621）"
  fi

  # 4. 枚举与类定义
  grep -q "SYS_VAR_ENABLE_FILE_SQL = 10754" src/share/system_variable/ob_sys_var_class_type.h \
    && ok "枚举 SYS_VAR_ENABLE_FILE_SQL = 10754" \
    || bad "枚举缺失"

  grep -q "ObSysVarEnableFileSql" src/sql/session/ob_system_variable_factory.h \
    && ok "类 ObSysVarEnableFileSql 已定义" \
    || bad "类定义缺失"

  # 5. session getter
  grep -q "get_enable_file_sql" src/sql/session/ob_basic_session_info.h \
    && ok "session getter get_enable_file_sql 已就位" \
    || bad "session getter 缺失"

  # 6. B3 的 WITH 选项解析（5d103c3 修复，从未验证）
  if grep -q "T_AI_COLUMN_OPTION_LIST" src/query/api/query/parser/ob_item_type.h; then
    ok "B3 AI COLUMN 选项类型已定义"
  else
    bad "B3 选项类型缺失"
  fi

  info "接下来跑：bash $0 build"
}

# ---------------------------------------------------------------- build
stage_build() {
  log "编译（release）"
  cd "${SRC_DIR}" || die "源码不在 ${SRC_DIR}"

  bison -V 2>/dev/null | head -1 || die "bison 缺失（必须 2.4.1）"
  clang --version 2>/dev/null | head -1 || die "clang 缺失"

  pkill -f "seekdb --base-dir" 2>/dev/null; sleep 2

  ./build.sh release --make -j8 > "${LOG_DIR}/build.log" 2>&1
  local rc=$?
  echo "  build exit=${rc}"
  if [ ${rc} -ne 0 ]; then
    grep -nE "error:|fatal error|Error [0-9]" "${LOG_DIR}/build.log" | head -20
    die "编译失败，日志 ${LOG_DIR}/build.log"
  fi
  [ -x "${SRC_DIR}/build_release/src/observer/seekdb" ] || die "产物不存在"
  ok "编译完成"
}

# ---------------------------------------------------------------- check
stage_check() {
  log "编译产物自查"

  cd "${SRC_DIR}" || die "源码不在 ${SRC_DIR}"

  [ -x build_release/src/observer/seekdb ] || die "还没编译，先跑 build"

  # 二进制里必须能看到变量名，否则运行时找不到
  if strings build_release/src/observer/seekdb | grep -q "^enable_file_sql$"; then
    ok "二进制里有 enable_file_sql"
  else
    bad "二进制里没有 enable_file_sql —— 编译没包含新变量"
  fi

  pkill -f "seekdb --base-dir" 2>/dev/null; sleep 2
  rm -rf /tmp/sdb && mkdir -p /tmp/sdb
  build_release/src/observer/seekdb --base-dir /tmp/sdb --port 2881 --nodaemon \
    > /tmp/sdb/server.log 2>&1 &
  sleep 20

  python3 - <<'PY'
import pymysql
try:
    c = pymysql.connect(host='127.0.0.1', port=2881, user='root', password='', autocommit=True)
    cur = c.cursor()
except Exception as e:
    print("  ✗ 连不上实例:", str(e)[:200]); raise SystemExit(1)

def q(sql):
    cur.execute(sql)
    return cur.fetchall()

try:
    r = q("select @@session.enable_file_sql")
    v = r[0][0]
    print(f"  ✓ select @@session.enable_file_sql = {v}")
    if v != 0:
        print("  ✗ 默认值应为 0（OFF）")
except Exception as e:
    print("  ✗ 读变量失败:", str(e)[:200])

try:
    q("set session enable_file_sql = ON")
    r = q("select @@session.enable_file_sql")
    print(f"  ✓ set ON 后 = {r[0][0]}")
    q("set session enable_file_sql = OFF")
    r = q("select @@session.enable_file_sql")
    print(f"  ✓ set OFF 后 = {r[0][0]}")
except Exception as e:
    print("  ✗ set 变量失败:", str(e)[:200])
PY

  info "接下来跑：bash $0 mysqltest"
}

# ---------------------------------------------------------------- mysqltest
run_case() {
  local suite="$1" case_name="$2"
  local dir="${SRC_DIR}/tools/deploy/mysql_test/test_suite/${suite}"
  local mt=""
  local c
  for c in "${SRC_DIR}/deps/3rd/u01/obclient/bin/mysqltest" \
           "${SRC_DIR}/deps/3rd/usr/local/oceanbase/client/bin/mysqltest" \
           "$(command -v mysqltest 2>/dev/null)"; do
    if [ -n "${c}" ] && [ -x "${c}" ]; then mt="${c}"; break; fi
  done
  [ -n "${mt}" ] || { bad "找不到 mysqltest，跳过 ${suite}/${case_name}"; return 1; }

  mkdir -p /tmp/mtr_var/tmp /tmp/mtr_var/log
  export LD_LIBRARY_PATH="${SRC_DIR}/deps/3rd/u01/obclient/lib:${LD_LIBRARY_PATH:-}"
  export OBSERVER_DIR=/tmp/sdb OBMYSQL_USR=root OBMYSQL_PWD= OBMYSQL_MS0=127.0.0.1 \
         OBMYSQL_PORT=2881 TENANT=mysql IS_BUSINESS=0

  "${mt}" --host=127.0.0.1 --port=2881 --user=root --password= --database=test \
    --tmpdir=/tmp/mtr_var/tmp --logdir=/tmp/mtr_var/log --silent --tail-lines=40 \
    --test-file="${dir}/t/${case_name}.test" \
    --result-file="${dir}/r/mysql/${case_name}.result" \
    --timer-file=/tmp/mtr_var/log/timer > "${LOG_DIR}/${suite}_${case_name}.log" 2>&1
  local rc=$?
  if [ ${rc} -eq 0 ]; then
    ok "${suite}/${case_name} PASS"
  else
    bad "${suite}/${case_name} FAIL (exit=${rc}) → ${LOG_DIR}/${suite}_${case_name}.log"
    tail -25 "${LOG_DIR}/${suite}_${case_name}.log" | sed 's/^/      /'
  fi
  return ${rc}
}

stage_mysqltest() {
  log "跑官方测例"

  # mock 服务
  pkill -f ai_column_mock.py 2>/dev/null; sleep 1
  pkill -f ai_function_mock.py 2>/dev/null; sleep 1
  cd "${SRC_DIR}/tools/deploy/mysql_test" || die "找不到 mysql_test"

  python3 test_suite/ai_column/include/ai_column_mock.py start 19120 > "${LOG_DIR}/mock.log" 2>&1
  python3 test_suite/ai_function/include/ai_function_mock.py start 8000 >> "${LOG_DIR}/mock.log" 2>&1
  sleep 2
  ok "mock 已启动"

  # 题2：file_sql（当前会失败，因为表函数还没实现）
  info "--- 题2 file_sql ---"
  run_case file_sql file_sql_basic

  # 题3：ai_column 五个用例
  info "--- 题3 ai_column ---"
  run_case ai_column ai_column_ddl
  run_case ai_column ai_column_t1_dml
  run_case ai_column ai_column_worker
  run_case ai_column ai_column_retry_strict
  run_case ai_column ai_column_vector_index

  # 题1：ai_function 回归
  info "--- 题1 ai_function 回归 ---"
  run_case ai_function ai_function_enhancement
}

case "${STAGE}" in
  sync)      stage_sync ;;
  build)     stage_build ;;
  check)     stage_check ;;
  mysqltest) stage_mysqltest ;;
  *)         echo "用法: bash $0 {sync|build|check|mysqltest}"; exit 1 ;;
esac
