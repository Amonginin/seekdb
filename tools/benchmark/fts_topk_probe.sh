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

# --------------------------------------------------------------- 6. 优化器是否真的下推了 LIMIT
# 这是本探针要回答的核心问题。query_limit 只取 20 行，却比全扫 8001 行的
# query_cn 还慢一倍。如果优化器把 LIMIT 留在子查询里没有变成 topk，
# 那就退化成「全扫 + 丢掉除前 20 行外的所有行」，这正是它慢的原因。
#
# 判定方法：看执行计划里有没有 LIMIT 节点，以及 table scan 上有没有
# topk / block max 的标记。
log "6. LIMIT 是否被下推成 topk（看执行计划里的 LIMIT 节点）"

plan_of() {
  ${MYSQL_VERBOSE} -e "EXPLAIN $1" 2>&1 | sed 's/^/    /'
}

echo "  6a. query_limit 的计划（子查询内 LIMIT 20）"
plan_of "SELECT COUNT(*) FROM (SELECT id FROM ${DB}.${TABLE} WHERE MATCH(content) AGAINST('${MATCH_WORDS}') LIMIT 20) t;"

echo "  6b. query_cn 的计划（同样 MATCH，无 LIMIT）"
plan_of "SELECT COUNT(*) FROM ${DB}.${TABLE} WHERE MATCH(content) AGAINST('${MATCH_WORDS}');"

# 计划里出现 "topk" 或 "block max" 说明走了 pushdown topk 路径；
# 只出现普通 LIMIT 节点说明 LIMIT 是在结果集上做的，没有下推。
echo "  6c. 计划中的关键标记"
plan_a=$(${MYSQL_VERBOSE} -e "EXPLAIN SELECT COUNT(*) FROM (SELECT id FROM ${DB}.${TABLE} WHERE MATCH(content) AGAINST('${MATCH_WORDS}') LIMIT 20) t;" 2>&1)
for kw in "topk" "TOPK" "block max" "BLOCK MAX" "LIMIT" "limit"; do
  n=$(printf '%s' "${plan_a}" | grep -c "${kw}")
  printf '    %-12s 出现 %s 次\n' "${kw}" "${n}"
done

# --------------------------------------------------------------- 7. 服务端是否真的少扫了行
# 这是 LIMIT 生效的最终证据：LIMIT 20 的查询，累计扫描行数应当远小于
# 无 LIMIT 的查询。直接从日志里抓最近一次两类查询的 rowscanned。
log "7. 服务端扫描行数对比（LIMIT 是否真的提前停了）"
if [ -d "${LOG_DIR}" ]; then
  echo "  7a. 最近 40 条 rowscanned"
  grep -rhoE "rowscanned=[0-9]+" "${LOG_DIR}" 2>/dev/null | tail -40 | sed 's/^/    /'
  echo "  7b. 服务端对 FTS 查询的摘要"
  grep -rhE "fts|retrieval|topk|block_max" "${LOG_DIR}" 2>/dev/null \
    | tail -15 | sed 's/.*] /    /' | cut -c1-200
else
  echo "  日志目录不存在: ${LOG_DIR}"
fi

log "完成"

# --------------------------------------------------------------- 8. 缓存命中与否
# query_cn 在平台上提升 53.5%，query_limit 只有 0.1%。两者都走 DaaT，都应该
# 命中倒排链缓存（本机日志里能看到 "fts inverted chain hit"）。如果 query_limit
# 的缓存没命中，说明它的执行路径确实不同 —— 这就是那 4~6 分的缺口所在。
log "8. 倒排链缓存命中情况（对比 query_cn 与 query_limit 的 token）"
if [ -d "${LOG_DIR}" ]; then
  echo "  8a. 所有缓存命中记录"
  grep -rh "fts inverted chain" "${LOG_DIR}" 2>/dev/null | tail -30 | sed 's/.*] /    /'
  echo "  8b. 命中次数统计"
  printf '    cached : %s\n' "$(grep -rh 'fts inverted chain cached' "${LOG_DIR}" 2>/dev/null | wc -l)"
  printf '    hit    : %s\n' "$(grep -rh 'fts inverted chain hit' "${LOG_DIR}" 2>/dev/null | wc -l)"
else
  echo "  日志目录不存在: ${LOG_DIR}"
fi
