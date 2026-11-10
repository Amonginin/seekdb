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
# 用法（实例必须已经在跑，端口 2881）：
#   bash tools/verify_file_sql.sh [case_name]
#
set -uo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SUITE="${ROOT}/tools/deploy/mysql_test/test_suite/file_sql"
WORK=/tmp/fsql_verify
OBSERVER_DIR=${OBSERVER_DIR:-/tmp/sdb}
DATA_DIR="${OBSERVER_DIR}/data_for_mysqltest_file_sql"
CASE=${1:-file_sql_basic}
LOG_DIR=/tmp/recover

mkdir -p "${LOG_DIR}"

MT=""
for c in "${ROOT}/deps/3rd/u01/obclient/bin/mysqltest" \
         "${ROOT}/deps/3rd/usr/local/oceanbase/client/bin/mysqltest"; do
  if [ -x "${c}" ]; then MT="${c}"; break; fi
done
if [ -z "${MT}" ]; then
  echo "✗ 找不到 mysqltest，先跑 bash tools/comp_recover.sh check"
  exit 1
fi

# 实例必须已经在跑；起实例交给 comp_recover.sh，避免两份逻辑打架。
if ! (exec 3<>/dev/tcp/127.0.0.1/2881) 2>/dev/null; then
  echo "✗ 2881 端口没在监听。先执行一次起库："
  echo "    bash tools/comp_recover.sh case file_sql/file_sql_basic"
  echo "  （它会失败，但实例会被拉起来）"
  exit 1
fi

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
echo "=== 被删掉的 --source 行 ==="
grep -c '^--source' "${SUITE}/t/${CASE}.test" | sed 's/^/  官方用例里 --source 行数: /'
echo "=== 跑 ${CASE} ==="

mkdir -p /tmp/mtr_var/tmp /tmp/mtr_var/log
export LD_LIBRARY_PATH="${ROOT}/deps/3rd/u01/obclient/lib:${LD_LIBRARY_PATH:-}"
export OBSERVER_DIR OBMYSQL_USR=root OBMYSQL_PWD= OBMYSQL_MS0=127.0.0.1 \
       OBMYSQL_PORT=2881 TENANT=mysql IS_BUSINESS=0

timeout 240 "${MT}" --host=127.0.0.1 --port=2881 --user=root --password= --database=test \
  --tmpdir=/tmp/mtr_var/tmp --logdir=/tmp/mtr_var/log --silent --tail-lines=40 \
  --test-file="${WORK}/t/${CASE}.test" \
  --result-file="${WORK}/r/mysql/${CASE}.result" \
  --timer-file=/tmp/mtr_var/log/timer 2>&1 | tee "${LOG_DIR}/file_sql_${CASE}.log"
rc=${PIPESTATUS[0]}

echo "=== exit=${rc} ==="
if [ "${rc}" -ne 0 ]; then
  echo "--- diff（第一个不一致的位置在尾部） ---"
  grep -nE "^---|^@@|^[<>]|^[0-9]+:" "${LOG_DIR}/file_sql_${CASE}.log" | head -40
fi
exit ${rc}
