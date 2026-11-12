#!/usr/bin/env bash
# 诊断 query_limit（topk/BMW 路径）的时间构成。
#
# 背景：query_limit 的 SQL 是
#   SELECT COUNT(*) FROM (SELECT id FROM docs WHERE MATCH(content)
#                         AGAINST('倒排索引 tokenizer') LIMIT 20) t;
# 只取 20 行，却比全扫 8001 行的 query_cn 还慢一倍。
#
# 这个脚本回答三个问题：
#   1. LIMIT 是否真的让扫描提前停（对比 LIMIT 20 / 100 / 1000 / 无 LIMIT 的耗时）
#   2. topk 路径比 DaaT 路径多出的开销在哪（BMW 归并 / BlockMax / BM25 估算）
#   3. 缓存对 topk 路径是否有帮助（对比 BLOCK MAX 开/关）
#
# 用法：
#   MYSQL=... MYSQL_VERBOSE=... bash tools/benchmark/fts_topk_probe.sh

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
cd "${ROOT_DIR}" || exit 1

MYSQL="${MYSQL:-mysql}"
MYSQL_VERBOSE="${MYSQL_VERBOSE:-mysql}"
DB="${DB:-fts_large_bench}"
TABLE="${TABLE:-docs}"
MATCH_WORDS="${MATCH_WORDS:-倒排索引 tokenizer}"
QUERY_ROUNDS="${QUERY_ROUNDS:-100}"

log() { printf '\n=== %s ===\n' "$*"; }

mysql_exec() {
  # -N -s：不要列名、tab 分隔
  ${MYSQL} -e "$1" 2>/dev/null | tail -n +1
}

run_timed() {
  # 跑 N 轮，返回平均毫秒（整数微秒累加，避免依赖 bc）
  local sql="$1" rounds="$2" i=0 total=0 start end
  # 预热
  ${MYSQL} -e "${sql}" >/dev/null 2>&1
  for ((i = 0; i < rounds; i++)); do
    start=$(date +%s%N)
    ${MYSQL} -e "${sql}" >/dev/null 2>&1
    end=$(date +%s%N)
    total=$(( total + (end - start) / 1000 ))
  done
  # 保留三位小数：total 是微秒
  local us_per_round=$(( total / rounds ))
  printf '%d.%03d' $(( us_per_round / 1000 )) $(( us_per_round % 1000 ))
}

log "环境"
echo "  mysql: ${MYSQL}"
echo "  rounds: ${QUERY_ROUNDS}"
mysql_exec "SELECT VERSION();" | sed 's/^/  version: /'
mysql_exec "SELECT COUNT(*) FROM ${DB}.${TABLE};" | sed 's/^/  rows: /'

# --------------------------------------------------------------- 1. LIMIT 生效性
log "1. LIMIT 是否让扫描提前停（同一 MATCH，只改 LIMIT）"
for lim in 20 100 1000 8001 0; do
  if [ "${lim}" -eq 0 ]; then
    sql="SELECT COUNT(*) FROM (SELECT id FROM ${DB}.${TABLE} WHERE MATCH(content) AGAINST('${MATCH_WORDS}')) t;"
    label="无 LIMIT (8001 行)"
  else
    sql="SELECT COUNT(*) FROM (SELECT id FROM ${DB}.${TABLE} WHERE MATCH(content) AGAINST('${MATCH_WORDS}') LIMIT ${lim}) t;"
    label="LIMIT ${lim}"
  fi
  hits=$(mysql_exec "${sql}" | tail -1)
  ms=$(run_timed "${sql}" "${QUERY_ROUNDS}")
  printf '  %-18s hits=%-8s avg=%.4f ms\n' "${label}" "${hits:-?}" "${ms}"
done

# --------------------------------------------------------------- 2. topk vs daat
log "2. 顶层 LIMIT（子查询外加 LIMIT）是否走 topk 路径"
# 如果优化器把 LIMIT 提到外层，走的就是 topk/BMW；
# 如果下推到内层子查询，就退化成全扫 + LIMIT。
for shape in \
  "SELECT COUNT(*) FROM (SELECT id FROM ${DB}.${TABLE} WHERE MATCH(content) AGAINST('${MATCH_WORDS}') LIMIT 20) t;" \
  "SELECT COUNT(*) FROM (SELECT id FROM ${DB}.${TABLE} WHERE MATCH(content) AGAINST('${MATCH_WORDS}')) t LIMIT 20;" \
  "SELECT id FROM ${DB}.${TABLE} WHERE MATCH(content) AGAINST('${MATCH_WORDS}') LIMIT 20;" ; do
  ms=$(run_timed "${shape}" "${QUERY_ROUNDS}")
  hits=$(mysql_exec "${shape}" | tail -1)
  printf '  avg=%.4f ms  hits=%-8s  %s\n' "${ms}" "${hits:-?}" "${shape:0:70}"
done

# --------------------------------------------------------------- 3. 纯 MATCH 计数
log "3. 对照：MATCH 命中总数（无 LIMIT、无子查询）"
sql="SELECT COUNT(*) FROM ${DB}.${TABLE} WHERE MATCH(content) AGAINST('${MATCH_WORDS}');"
hits=$(mysql_exec "${sql}" | tail -1)
ms=$(run_timed "${sql}" "${QUERY_ROUNDS}")
printf '  avg=%.4f ms  hits=%s\n' "${ms}" "${hits}"

# --------------------------------------------------------------- 4. 计划
log "4. 执行计划（确认走哪种检索路径）"
${MYSQL_VERBOSE} -e "EXPLAIN SELECT COUNT(*) FROM (SELECT id FROM ${DB}.${TABLE} WHERE MATCH(content) AGAINST('${MATCH_WORDS}') LIMIT 20) t;" 2>&1 | sed 's/^/  /'

log "4b. EXPLAIN EXTENDED（看 topk / block max 标记）"
${MYSQL_VERBOSE} -e "EXPLAIN EXTENDED SELECT COUNT(*) FROM (SELECT id FROM ${DB}.${TABLE} WHERE MATCH(content) AGAINST('${MATCH_WORDS}') LIMIT 20) t;" 2>&1 | sed 's/^/  /'

log "4c. 对照：DaaT 路径的计划"
${MYSQL_VERBOSE} -e "EXPLAIN SELECT COUNT(*) FROM ${DB}.${TABLE} WHERE MATCH(content) AGAINST('${MATCH_WORDS}');" 2>&1 | sed 's/^/  /'

# --------------------------------------------------------------- 5. 扫描行数
log "5. 实际扫描行数（从 observer.log 数 rowscanned）"
LOG_DIR="${LOG_DIR:-/tmp/sdb/log}"
if [ -d "${LOG_DIR}" ]; then
  grep -rhoE "rowscanned=[0-9]+|rows=[0-9]+" "${LOG_DIR}" 2>/dev/null | tail -20 | sed 's/^/  /'
  echo "  (若为空，改用 perf schema 或 v$sql_monitor 采样)"
else
  echo "  日志目录不存在: ${LOG_DIR}"
fi

log "完成"
