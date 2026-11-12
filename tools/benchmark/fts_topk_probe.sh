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
MYSQL_VERBOSE="${MYSQL_VERBOSE:-${MYSQL}}"
DB="${DB:-fts_large_bench}"
TABLE="${TABLE:-docs}"
MATCH_WORDS="${MATCH_WORDS:-倒排索引 tokenizer}"
QUERY_ROUNDS="${QUERY_ROUNDS:-100}"

log() { printf '\n=== %s ===\n' "$*"; }

# Keep a copy of everything this run printed, so the summary at the end can
# quote the numbers instead of asking the reader to scroll back.
OUT_FILE="${OUT_FILE:-/tmp/fts_topk_probe_out.txt}"
: > "${OUT_FILE}"
exec > >(tee -a "${OUT_FILE}") 2>&1

# Count the statements that failed. A tee writer runs asynchronously, so
# grepping the file back would race with the last writes.
FAILED_STMTS=0

mysql_exec() {
  # -N -s：不要列名、tab 分隔
  ${MYSQL} -e "$1" 2>/dev/null | tail -n +1
}

run_timed() {
  # 跑 N 轮，返回平均毫秒（整数微秒累加，避免依赖 bc）
  local sql="$1" rounds="$2" i=0 total=0 start end
  # 预热。失败要立刻发现：客户端返回错误时仍会花掉时间，把错误路径的耗时
  # 当成结果记下来，是这轮探针一开始读到的假数据。
  if ! ${MYSQL} -e "${sql}" >/dev/null 2>&1; then
    echo "ERROR"
    return 1
  fi
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

# The probe measures timings, so it must refuse to run against a table that is
# not set up. Without the index every MATCH fails with "Can't find FULLTEXT
# index", and the timings that come back are the cost of the error path: fast,
# meaningless and easy to misread as a result.
#
# Check the database, the rows and the index before anything else, and stop
# with a message that says what is missing.
if ! mysql_exec "SELECT COUNT(*) FROM ${DB}.${TABLE};" | tail -1 | grep -qE '^[0-9]+$'; then
  echo "!! 表 ${DB}.${TABLE} 不存在或不可读。先跑 tools/comp_recover.sh bench clean 建好数据。" >&2
  exit 1
fi
rows=$(mysql_exec "SELECT COUNT(*) FROM ${DB}.${TABLE};" | tail -1)
echo "  rows: ${rows}"
if [[ "${rows}" -eq 0 ]]; then
  echo "!! 表 ${DB}.${TABLE} 是空的。先跑 tools/comp_recover.sh bench clean 灌数据。" >&2
  exit 1
fi

# The probe matches on ${TABLE}.content, so the index has to cover that column.
idx_list=$(mysql_exec "SHOW INDEX FROM ${DB}.${TABLE};" | awk '{print $3}' | sort -u | tr '\n' ' ')
echo "  indexes: ${idx_list}"
if ! mysql_exec "SHOW INDEX FROM ${DB}.${TABLE};" | grep -q "fti_content_ik"; then
  echo "!! 缺少全文索引 fti_content_ik（MATCH(content) 需要它）。" >&2
  echo "   先跑 tools/comp_recover.sh bench clean 建好索引。" >&2
  exit 1
fi

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
  if [[ "${hits}" =~ ^[0-9]+$ ]]; then
    printf '  %-18s hits=%-8s avg=%s ms\n' "${label}" "${hits}" "${ms}"
  else
    FAILED_STMTS=$((FAILED_STMTS + 1))
    printf '  %-18s hits=FAIL   avg=%s ms   <== 无效，语句执行失败\n' "${label}" "${ms}"
  fi
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
  if [[ "${hits}" =~ ^[0-9]+$ ]]; then
    printf '  avg=%s ms  hits=%-8s  %s\n' "${ms}" "${hits}" "${shape:0:70}"
  else
    FAILED_STMTS=$((FAILED_STMTS + 1))
    printf '  avg=%s ms  hits=FAIL   %s   <== 无效\n' "${ms}" "${shape:0:70}"
  fi
done

# --------------------------------------------------------------- 3. 纯 MATCH 计数
log "3. 对照：MATCH 命中总数（无 LIMIT、无子查询）"
sql="SELECT COUNT(*) FROM ${DB}.${TABLE} WHERE MATCH(content) AGAINST('${MATCH_WORDS}');"
hits=$(mysql_exec "${sql}" | tail -1)
ms=$(run_timed "${sql}" "${QUERY_ROUNDS}")
if [[ "${hits}" =~ ^[0-9]+$ ]]; then
  printf '  avg=%s ms  hits=%s\n' "${ms}" "${hits}"
else
  printf '  avg=%s ms  hits=FAIL   <== 无效，语句执行失败\n' "${ms}"
fi

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
if [ -d "${LOG_DIR}" ]; then  grep -rhoE "rowscanned=[0-9]+|rows=[0-9]+" "${LOG_DIR}" 2>/dev/null | tail -20 | sed 's/^/  /'
  echo "  (若为空，改用 perf schema 或 v\$sql_monitor 采样)"
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

# --------------------------------------------------------------- 9. 成本来自 token 数还是行数
# 这是本探针要回答的最终问题。
#
# 已经测到的事实：
#   LIMIT 20   -> 8.10 ms
#   LIMIT 100  -> 8.44 ms
#   LIMIT 1000 -> 12.94 ms
#   LIMIT 8001 -> 32.52 ms
#   裸 MATCH    -> 7.12 ms
#
# LIMIT 20 和 LIMIT 100 几乎一样，而它们都比裸 MATCH 慢。这说明：
#   a) LIMIT 确实生效（20 比 8001 快 4 倍）
#   b) LIMIT 省不掉 MATCH 本身的扫描成本
#
# MATCH 的查询词被切成多个 token，每个 token 都要扫完自己的 posting list，
# 归并才能产出第一个交集行。所以如果 token 数才是成本来源，那么：
#   单 token 的 MATCH + LIMIT 20 应当明显快于双 token 的 MATCH + LIMIT 20。
# 这一段直接测这个对比。
log "9. 成本来自 query token 数还是结果行数"

timed_hits() {
  # $1=sql $2=label
  local sql="$1" label="$2" hits ms
  hits=$(mysql_exec "${sql}" | tail -1)
  ms=$(run_timed "${sql}" "${QUERY_ROUNDS}")
  if [[ "${hits}" =~ ^[0-9]+$ ]]; then
    printf '  %-34s hits=%-8s avg=%s ms\n' "${label}" "${hits}" "${ms}"
  else
    FAILED_STMTS=$((FAILED_STMTS + 1))
    printf '  %-34s hits=FAIL   avg=%s ms   <== 无效，语句执行失败\n' "${label}" "${ms}"
  fi
}

echo "  9a. 同样的 LIMIT 20，query token 数不同"
timed_hits "SELECT COUNT(*) FROM (SELECT id FROM ${DB}.${TABLE} WHERE MATCH(content) AGAINST('倒排索引') LIMIT 20) t;" \
           "单 token（倒排索引）"
timed_hits "SELECT COUNT(*) FROM (SELECT id FROM ${DB}.${TABLE} WHERE MATCH(content) AGAINST('倒排索引 tokenizer') LIMIT 20) t;" \
           "双 token（倒排索引 tokenizer）"
timed_hits "SELECT COUNT(*) FROM (SELECT id FROM ${DB}.${TABLE} WHERE MATCH(content) AGAINST('倒排索引 tokenizer 索引') LIMIT 20) t;" \
           "三 token"

echo "  9b. 同样的 query token，不同 LIMIT"
timed_hits "SELECT COUNT(*) FROM (SELECT id FROM ${DB}.${TABLE} WHERE MATCH(content) AGAINST('倒排索引') LIMIT 20) t;" \
           "单 token LIMIT 20"
timed_hits "SELECT COUNT(*) FROM (SELECT id FROM ${DB}.${TABLE} WHERE MATCH(content) AGAINST('倒排索引') LIMIT 100) t;" \
           "单 token LIMIT 100"
timed_hits "SELECT COUNT(*) FROM (SELECT id FROM ${DB}.${TABLE} WHERE MATCH(content) AGAINST('倒排索引') LIMIT 8001) t;" \
           "单 token LIMIT 8001"
timed_hits "SELECT COUNT(*) FROM ${DB}.${TABLE} WHERE MATCH(content) AGAINST('倒排索引');" \
           "单 token 无 LIMIT"

echo "  9c. 单列 vs 双列 MATCH"
timed_hits "SELECT COUNT(*) FROM (SELECT id FROM ${DB}.${TABLE} WHERE MATCH(content) AGAINST('倒排索引 tokenizer') LIMIT 20) t;" \
           "MATCH(content)"
timed_hits "SELECT COUNT(*) FROM (SELECT id FROM ${DB}.${TABLE} WHERE MATCH(title, content) AGAINST('倒排索引 tokenizer') LIMIT 20) t;" \
           "MATCH(title, content)"

# --------------------------------------------------------------- 10. 本轮结果有效性
# 一次性把「哪些数字能用」说清楚，而不是让读的人自己逐行判断。
log "10. 本轮结果有效性"

fail_count="${FAILED_STMTS}"

echo "  10a. 语句失败次数"
printf '    hits=FAIL 出现 %s 次\n' "${fail_count}"
if [[ "${fail_count}" -gt 0 ]]; then
  echo "    >>> 本轮有语句执行失败，上面的耗时是错误路径的耗时，不能用。"
  echo "    >>> 失败原因通常是索引缺失，先确认 build 是否成功。"
fi

echo "  10b. 关键结论"
if [[ "${fail_count}" -eq 0 ]]; then
  echo "    所有语句均成功，下面的耗时可用。"
fi

# 把关键对照拉出来，省得从长输出里找
echo "  10c. LIMIT 生效性（同一 MATCH，只改 LIMIT）"
grep -E "LIMIT (20|100|1000|8001) |无 LIMIT" /tmp/fts_topk_probe_out.txt 2>/dev/null | sed 's/^/    /'

echo "  10d. token 数 vs 行数"
grep -E "token（|token LIMIT|token 无 LIMIT|MATCH\(" /tmp/fts_topk_probe_out.txt 2>/dev/null | sed 's/^/    /'

echo "  10e. 倒排链缓存"
grep -E "cached :|hit    :" /tmp/fts_topk_probe_out.txt 2>/dev/null | sed 's/^/    /'

# --------------------------------------------------------------- 11. 排序是不是那 4 倍的来源
# 已经测到的事实：
#   单 token 无 LIMIT    6.169 ms   (hits=6667)
#   单 token LIMIT 8001  25.757 ms  (hits=6667)  <- 同一个结果集，慢 4 倍
#
# 同一个结果集，慢了 4 倍，所以多出来的不是扫描，是别的开销。执行计划里
# LIMIT 形式的查询带着 "sort_keys([MATCH(...)], DESC)"，而 MATCH 的 relevance
# 是要算 BM25 的。
#
# 这一段直接对比三种形态：
#   A) WHERE MATCH，不排序不取行      -> 只要布尔结果
#   B) WHERE MATCH，LIMIT（无 ORDER BY）
#   C) WHERE MATCH + 显式 ORDER BY relevance
# 如果 B 明显慢于 A 而和 C 一样慢，那开销就在 relevance 计算与排序上。
log "11. 排序 / relevance 是不是额外开销的来源"

sorted_sql="SELECT COUNT(*) FROM (SELECT id FROM ${DB}.${TABLE} WHERE MATCH(content) AGAINST('${MATCH_WORDS}') LIMIT 20) t;"
plain_sql="SELECT COUNT(*) FROM ${DB}.${TABLE} WHERE MATCH(content) AGAINST('${MATCH_WORDS}');"

echo "  11a. 不排序、不取行（只要布尔结果）"
timed_hits "${plain_sql}" "WHERE MATCH 计数"

echo "  11b. LIMIT 20（计划里带 MATCH 排序键）"
timed_hits "${sorted_sql}" "WHERE MATCH + LIMIT 20"

echo "  11c. 显式按 relevance 排序"
timed_hits "SELECT COUNT(*) FROM (SELECT id FROM ${DB}.${TABLE} WHERE MATCH(content) AGAINST('${MATCH_WORDS}') ORDER BY MATCH(content) AGAINST('${MATCH_WORDS}') DESC LIMIT 20) t;" \
           "WHERE MATCH + ORDER BY relevance"

echo "  11d. 去掉 relevance 的写法（只取行号，不碰 MATCH 表达式）"
timed_hits "SELECT COUNT(*) FROM (SELECT id FROM ${DB}.${TABLE} WHERE MATCH(content) AGAINST('${MATCH_WORDS}') LIMIT 20) t;" \
           "LIMIT 20 取 id（对照）"

echo "  11e. 计划里的 relevance 标记"
for q in "${plain_sql}" "${sorted_sql}"; do
  plan=$(mysql_exec "EXPLAIN ${q}" 2>&1 | tr '\n' ' ')
  if printf '%s' "${plan}" | grep -qi "calc_relevance=true"; then
    printf '    calc_relevance=true   <- %s\n' "${q:0:60}"
  elif printf '%s' "${plan}" | grep -qi "calc_relevance"; then
    printf '    calc_relevance=false  <- %s\n' "${q:0:60}"
  else
    printf '    计划里没有 calc_relevance 标记  <- %s\n' "${q:0:60}"
  fi
done

log "完成（全部）"

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
