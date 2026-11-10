#!/usr/bin/env bash
#
# 本地跑 file_sql 用例。
#
# 官方 file_sql_basic.test 头两行是：
#   --source mysql_test/test_suite/file_sql/include/copy_file_sql_data.inc
# 而这个 include 要求两样本地环境没有的东西：
#   1) seekdb 里有 admin 账号（`connect ... admin ...`）；
#   2) 能用 ssh + scp 把样例文件拷到 observer 机器上。
# 判题环境有这两样，魔搭上没有，于是用例会在第 7 行就 1045 / ssh 失败，
# 根本没走到被验证的 SQL。
#
# 这个脚本不改任何官方文件：它把用例复制到 /tmp，只删掉两条 --source，
# 自己把 data/ 下的样例放到 $OBSERVER_DIR/data_for_mysqltest_file_sql
# （用例里本来就有 `let $FILE_SQL_DATA_DIR = $OBSERVER_DIR/...` 这一行）。
#
# 每次都用新编译的二进制重启实例 —— 否则测的是上一次编译的进程，
# 会出现「明明改了代码，报错一字不差」的假阴性（踩过）。
#
# 用法：
#   bash tools/verify_file_sql.sh [case_name]
#
set -uo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SUITE="${ROOT}/tools/deploy/mysql_test/test_suite/file_sql"
WORK=/tmp/fsql_verify
OBSERVER_DIR=${OBSERVER_DIR:-/tmp/sdb}
DATA_DIR="${OBSERVER_DIR}/data_for_mysqltest_file_sql"
CASE=${1:-file_sql_basic}
BIN="${ROOT}/build_release/src/observer/seekdb"
CLIENT_DIR="${ROOT}/deps/3rd/u01/obclient/bin"
LOG_DIR=/tmp/recover
PORT=2881

mkdir -p "${LOG_DIR}"

MT=""
for c in "${CLIENT_DIR}/mysqltest" \
         "${ROOT}/deps/3rd/usr/local/oceanbase/client/bin/mysqltest"; do
  if [ -x "${c}" ]; then MT="${c}"; break; fi
done
if [ -z "${MT}" ]; then
  echo "✗ 找不到 mysqltest，先跑 bash tools/comp_recover.sh check"
  exit 1
fi
if [ ! -x "${BIN}" ]; then
  echo "✗ 找不到二进制 ${BIN}，先编译"
  exit 1
fi

port_open() { (exec 3<>/dev/tcp/127.0.0.1/${PORT}) 2>/dev/null; }

# ---------------------------------------------------------------- 重启实例
echo "=== 重启实例（保证跑的是刚编出来的二进制） ==="
pkill -f "seekdb --base-dir ${OBSERVER_DIR}" 2>/dev/null
for _ in $(seq 1 30); do port_open || break; sleep 1; done
if port_open; then
  echo "✗ ${PORT} 仍被占用，先手工结束残留进程"
  exit 1
fi

rm -rf "${OBSERVER_DIR}"
mkdir -p "${OBSERVER_DIR}"
cd "${OBSERVER_DIR}" || exit 1
nohup "${BIN}" --base-dir "${OBSERVER_DIR}" --port "${PORT}" --nodaemon \
  > "${OBSERVER_DIR}/server.log" 2>&1 &
for _ in $(seq 1 90); do port_open && break; sleep 1; done
if ! port_open; then
  echo "✗ 实例起不来，看 ${OBSERVER_DIR}/server.log"
  tail -20 "${OBSERVER_DIR}/server.log"
  exit 1
fi
echo "  ✓ 实例已就绪（新二进制）"

# 全新 bootstrap 的实例没有 test 库，mysqltest 用 --database=test 连接。
export LD_LIBRARY_PATH="${ROOT}/deps/3rd/u01/obclient/lib:${LD_LIBRARY_PATH:-}"
for cli in "${CLIENT_DIR}/obclient" "${CLIENT_DIR}/mysql"; do
  if [ -x "${cli}" ]; then
    if "${cli}" -h127.0.0.1 -P${PORT} -uroot --protocol=tcp \
         -e "create database if not exists test;" >/dev/null 2>&1; then
      echo "  ✓ test 库已就绪"
      break
    fi
  fi
done

# ---------------------------------------------------------------- 布置样例
rm -rf "${WORK}"
mkdir -p "${WORK}/t" "${WORK}/r/mysql" "${DATA_DIR}"
cp "${SUITE}"/data/* "${DATA_DIR}"/
if [ -f "${DATA_DIR}/simple.parquet.b64" ]; then
  base64 -d "${DATA_DIR}/simple.parquet.b64" > "${DATA_DIR}/simple.parquet"
  rm -f "${DATA_DIR}/simple.parquet.b64"
fi

sed -e '/^--source .*copy_file_sql_data\.inc/d' \
    -e '/^--source .*remove_file_sql_data\.inc/d' \
    "${SUITE}/t/${CASE}.test" > "${WORK}/t/${CASE}.test"
cp "${SUITE}/r/mysql/${CASE}.result" "${WORK}/r/mysql/"

echo "=== 样例目录 ${DATA_DIR} ==="
ls -1 "${DATA_DIR}"
echo "=== 跑 ${CASE} ==="

mkdir -p /tmp/mtr_var/tmp /tmp/mtr_var/log
export OBSERVER_DIR OBMYSQL_USR=root OBMYSQL_PWD= OBMYSQL_MS0=127.0.0.1 \
       OBMYSQL_PORT=${PORT} TENANT=mysql IS_BUSINESS=0

timeout 240 "${MT}" --host=127.0.0.1 --port=${PORT} --user=root --password= --database=test \
  --tmpdir=/tmp/mtr_var/tmp --logdir=/tmp/mtr_var/log --silent --tail-lines=40 \
  --test-file="${WORK}/t/${CASE}.test" \
  --result-file="${WORK}/r/mysql/${CASE}.result" \
  --timer-file=/tmp/mtr_var/log/timer 2>&1 | tee "${LOG_DIR}/file_sql_${CASE}.log"
rc=${PIPESTATUS[0]}

echo "=== exit=${rc} ==="
if [ "${rc}" -ne 0 ]; then
  echo "--- diff（第一个不一致的位置在尾部） ---"
  grep -nE "^---|^@@|^[<>]|^[0-9]+:" "${LOG_DIR}/file_sql_${CASE}.log" | head -40
  echo "--- 服务端日志里的相关行 ---"
  grep -nE "file table|file sql|parse csv|read file|generated file" \
    "${OBSERVER_DIR}/log/seekdb.log" 2>/dev/null | tail -20
fi
exit ${rc}
