#!/usr/bin/env bash
# 大规模全文索引压测：raw load + FTS index build + TOKENIZE + MATCH 查询
#
# 用于优化前后 seekdb 对比。建议固定 ROWS/BATCH/ROUNDS/QUERY_ROUNDS/SAMPLES，
# 分别打 LABEL，并在相同机器负载下重复运行。
#
# 用法:
#   ./tools/benchmark/fts_large_bench.sh
#   LABEL=before ROWS=50000 ./tools/benchmark/fts_large_bench.sh
#   LABEL=after SKIP_LOAD=1 ./tools/benchmark/fts_large_bench.sh
#   OUTPUT=./bench_result.txt LABEL=after ./tools/benchmark/fts_large_bench.sh
#
# 环境变量:
#   MYSQL          mysql 客户端命令（默认 -h127.0.0.1 -P2881 -uroot -N -s --default-character-set=utf8mb4）
#   MYSQL_VERBOSE  mysql 客户端命令，保留普通输出（默认 -h127.0.0.1 -P2881 -uroot --default-character-set=utf8mb4）
#   LABEL          结果标签，如 before / after（默认 unknown）
#   ROWS           插入文档数（默认 20000）
#   BATCH          每批 INSERT 行数（默认 500）
#   ROUNDS         TOKENIZE 压测轮次（默认 3000）
#   QUERY_ROUNDS   每条 MATCH SQL 每个 sample 重复次数（默认 200）
#   SAMPLES        TOKENIZE/MATCH 采样组数（默认 3）
#   WARMUP         TOKENIZE/MATCH 每项预热轮次（默认 30）
#   SKIP_LOAD      设为 1 跳过建库、灌数、建索引，仅跑分词/查询压测
#   OUTPUT         结果追加写入的文件（可选）

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# 镜像里通常没有 mysql 客户端，只有 obclient：没显式给 MYSQL/MYSQL_VERBOSE 时自己挑一个。
if [[ -z "${MYSQL:-}" || -z "${MYSQL_VERBOSE:-}" ]]; then
  if command -v mysql >/dev/null 2>&1; then
    _default_client="mysql -h127.0.0.1 -P2881 -uroot -N -s --default-character-set=utf8mb4"
    _default_client_verbose="mysql -h127.0.0.1 -P2881 -uroot --default-character-set=utf8mb4"
  elif command -v obclient >/dev/null 2>&1; then
    _default_client="obclient -h127.0.0.1 -P2881 -uroot -N -s --protocol=tcp --default-character-set=utf8mb4"
    _default_client_verbose="obclient -h127.0.0.1 -P2881 -uroot --protocol=tcp --default-character-set=utf8mb4"
  else
    _default_client="mysql -h127.0.0.1 -P2881 -uroot -N -s --default-character-set=utf8mb4"
    _default_client_verbose="mysql -h127.0.0.1 -P2881 -uroot --default-character-set=utf8mb4"
  fi
  MYSQL="${MYSQL:-${_default_client}}"
  MYSQL_VERBOSE="${MYSQL_VERBOSE:-${_default_client_verbose}}"
fi
LABEL="${LABEL:-unknown}"
ROWS="${ROWS:-20000}"
BATCH="${BATCH:-500}"
ROUNDS="${ROUNDS:-3000}"
QUERY_ROUNDS="${QUERY_ROUNDS:-200}"
SAMPLES="${SAMPLES:-3}"
WARMUP="${WARMUP:-30}"
SKIP_LOAD="${SKIP_LOAD:-0}"
OUTPUT="${OUTPUT:-}"

IK_TEXT="OceanBase是一款非常稳定的数据库，全文索引的分词器热路径优化包括解析器实例复用、停用词全局单例、内存池化数据结构等技术，在大量文档索引和查询场景下可以显著降低 CPU 开销。"
BENG_TEXT="The quick brown fox jumps over the lazy dog. Full-text search indexing requires efficient tokenization on the hot path."

declare -A METRICS
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/fts_large_bench.XXXXXX")"

cleanup() { rm -rf "${TMP_DIR}"; }
trap cleanup EXIT

log() { echo "$@" >&2; }

now_ns() { date +%s%N; }

elapsed_sec() {
  local start_ns="$1" end_ns="$2"
  awk "BEGIN {printf \"%.3f\", (${end_ns} - ${start_ns}) / 1000000000}"
}

elapsed_ms() {
  local start_ns="$1" end_ns="$2"
  awk "BEGIN {printf \"%.3f\", (${end_ns} - ${start_ns}) / 1000000}"
}

mysql_exec() { $MYSQL -e "SET NAMES utf8mb4; SET ob_query_timeout = 100000000000; SET ob_trx_timeout = 100000000000; $1"; }

validate_positive_int() {
  local name="$1" value="$2"
  if [[ ! "${value}" =~ ^[1-9][0-9]*$ ]]; then
    log "ERROR: ${name} must be a positive integer, got '${value}'"
    exit 1
  fi
}

write_session_header() {
  local file="$1"
  {
    echo "SET NAMES utf8mb4;"
    echo "SET ob_query_timeout = 100000000000;"
    echo "SET ob_trx_timeout = 100000000000;"
  } > "${file}"
}

append_repeated_sql() {
  local file="$1" rounds="$2" sql="$3"
  local i
  for ((i = 0; i < rounds; i++)); do
    echo "${sql}" >> "${file}"
  done
}

calc_sample_stats() {
  local file="$1"
  awk '
    {
      a[NR] = $1
      sum += $1
      sumsq += $1 * $1
    }
    END {
      if (NR == 0) {
        printf "N/A N/A N/A N/A N/A N/A"
        exit
      }
      for (i = 1; i <= NR; i++) {
        for (j = i + 1; j <= NR; j++) {
          if (a[i] > a[j]) {
            t = a[i]; a[i] = a[j]; a[j] = t
          }
        }
      }
      mean = sum / NR
      var = sumsq / NR - mean * mean
      if (var < 0) {
        var = 0
      }
      median = (NR % 2) ? a[(NR + 1) / 2] : (a[NR / 2] + a[NR / 2 + 1]) / 2
      p95_idx = int(NR * 0.95 + 0.999999)
      if (p95_idx < 1) {
        p95_idx = 1
      } else if (p95_idx > NR) {
        p95_idx = NR
      }
      printf "%.4f %.4f %.4f %.4f %.4f %.4f", mean, median, sqrt(var), a[1], a[NR], a[p95_idx]
    }
  ' "${file}"
}

record_sample_stats() {
  local name="$1" sample_file="$2"
  local mean median stdev min max p95
  read -r mean median stdev min max p95 <<< "$(calc_sample_stats "${sample_file}")"
  METRICS["${name}_avg_ms"]="${mean}"
  METRICS["${name}_median_ms"]="${median}"
  METRICS["${name}_stdev_ms"]="${stdev}"
  METRICS["${name}_min_ms"]="${min}"
  METRICS["${name}_max_ms"]="${max}"
  METRICS["${name}_p95_ms"]="${p95}"
}

run_repeated_sql_bench() {
  local name="$1" rounds="$2" sql="$3"
  local warmup_sql="${TMP_DIR}/${name}.warmup.sql"
  local sample_file="${TMP_DIR}/${name}.samples"
  local sample sql_file start_ns end_ns total_ms avg_ms

  : > "${sample_file}"

  if (( WARMUP > 0 )); then
    write_session_header "${warmup_sql}"
    append_repeated_sql "${warmup_sql}" "${WARMUP}" "${sql}"
    $MYSQL < "${warmup_sql}" >/dev/null
  fi

  for ((sample = 1; sample <= SAMPLES; sample++)); do
    sql_file="${TMP_DIR}/${name}.${sample}.sql"
    write_session_header "${sql_file}"
    append_repeated_sql "${sql_file}" "${rounds}" "${sql}"
    start_ns=$(now_ns)
    $MYSQL < "${sql_file}" >/dev/null
    end_ns=$(now_ns)
    total_ms=$(elapsed_ms "${start_ns}" "${end_ns}")
    avg_ms=$(awk "BEGIN {printf \"%.4f\", ${total_ms} / ${rounds}}")
    echo "${avg_ms}" >> "${sample_file}"
    log "  ${name}: sample=${sample}/${SAMPLES} rounds=${rounds} total_ms=${total_ms} avg_ms=${avg_ms}"
  done

  record_sample_stats "${name}" "${sample_file}"
}

run_tokenize_bench() {
  local name="$1" parser="$2" text="$3"
  run_repeated_sql_bench "${name}" "${ROUNDS}" "SELECT tokenize('${text}', '${parser}');"
}

run_query_bench() {
  local name="$1" sql="$2"
  local cnt
  cnt=$(mysql_exec "${sql}" | tail -1)
  : "${cnt:=0}"
  METRICS["${name}_hits"]="${cnt}"
  log "  ${name}: warmup_hits=${cnt}"
  run_repeated_sql_bench "${name}" "${QUERY_ROUNDS}" "${sql}"
}

time_mysql_command() {
  local name="$1" sql="$2"
  local start_ns end_ns total_sec
  start_ns=$(now_ns)
  mysql_exec "${sql}" >/dev/null
  end_ns=$(now_ns)
  total_sec=$(elapsed_sec "${start_ns}" "${end_ns}")
  METRICS["${name}_sec"]="${total_sec}"
  log "  ${name}: ${total_sec}s"
}

write_report() {
  local ts git_head git_dirty
  ts="$(date '+%Y-%m-%d %H:%M:%S')"
  git_head="$(git -C "${SCRIPT_DIR}/../.." rev-parse --short HEAD 2>/dev/null || echo "unknown")"
  if git -C "${SCRIPT_DIR}/../.." diff --quiet --ignore-submodules -- 2>/dev/null; then
    git_dirty="0"
  else
    git_dirty="1"
  fi

  {
    echo "========================================"
    echo "FTS Large Benchmark Report"
    echo "========================================"
    echo "timestamp:              ${ts}"
    echo "label:                  ${LABEL}"
    echo "git_head:               ${git_head}"
    echo "git_dirty:              ${git_dirty}"
    echo "rows:                   ${ROWS}"
    echo "batch:                  ${BATCH}"
    echo "rounds:                 ${ROUNDS}"
    echo "query_rounds:           ${QUERY_ROUNDS}"
    echo "samples:                ${SAMPLES}"
    echo "warmup:                 ${WARMUP}"
    echo "skip_load:              ${SKIP_LOAD}"
    echo "----------------------------------------"
    echo "select1_avg_ms:         ${METRICS[select1_avg_ms]:-N/A}"
    echo "select1_stdev_ms:       ${METRICS[select1_stdev_ms]:-N/A}"
    echo "raw_load_sec:           ${METRICS[raw_load_sec]:-N/A}"
    echo "raw_load_rows_per_sec:  ${METRICS[raw_load_rows_per_sec]:-N/A}"
    echo "build_ik_all_sec:       ${METRICS[build_ik_all_sec]:-N/A}"
    echo "build_ik_content_sec:   ${METRICS[build_ik_content_sec]:-N/A}"
    echo "build_beng_en_sec:      ${METRICS[build_beng_en_sec]:-N/A}"
    echo "build_total_sec:        ${METRICS[build_total_sec]:-N/A}"
    echo "----------------------------------------"
    echo "tokenize_ik_avg_ms:     ${METRICS[tokenize_ik_avg_ms]:-N/A}"
    echo "tokenize_ik_median_ms:  ${METRICS[tokenize_ik_median_ms]:-N/A}"
    echo "tokenize_ik_stdev_ms:   ${METRICS[tokenize_ik_stdev_ms]:-N/A}"
    echo "tokenize_beng_avg_ms:   ${METRICS[tokenize_beng_avg_ms]:-N/A}"
    echo "tokenize_beng_median_ms:${METRICS[tokenize_beng_median_ms]:-N/A}"
    echo "tokenize_beng_stdev_ms: ${METRICS[tokenize_beng_stdev_ms]:-N/A}"
    echo "----------------------------------------"
    echo "query_cn_hits:          ${METRICS[query_cn_hits]:-N/A}"
    echo "query_cn_avg_ms:        ${METRICS[query_cn_avg_ms]:-N/A}"
    echo "query_cn_stdev_ms:      ${METRICS[query_cn_stdev_ms]:-N/A}"
    echo "query_beng_hits:        ${METRICS[query_beng_hits]:-N/A}"
    echo "query_beng_avg_ms:      ${METRICS[query_beng_avg_ms]:-N/A}"
    echo "query_beng_stdev_ms:    ${METRICS[query_beng_stdev_ms]:-N/A}"
    echo "query_mixed_hits:       ${METRICS[query_mixed_hits]:-N/A}"
    echo "query_mixed_avg_ms:     ${METRICS[query_mixed_avg_ms]:-N/A}"
    echo "query_mixed_stdev_ms:   ${METRICS[query_mixed_stdev_ms]:-N/A}"
    echo "query_limit_hits:       ${METRICS[query_limit_hits]:-N/A}"
    echo "query_limit_avg_ms:     ${METRICS[query_limit_avg_ms]:-N/A}"
    echo "query_limit_stdev_ms:   ${METRICS[query_limit_stdev_ms]:-N/A}"
    echo "========================================"
  }
}

validate_positive_int "ROWS" "${ROWS}"
validate_positive_int "BATCH" "${BATCH}"
validate_positive_int "ROUNDS" "${ROUNDS}"
validate_positive_int "QUERY_ROUNDS" "${QUERY_ROUNDS}"
validate_positive_int "SAMPLES" "${SAMPLES}"

if [[ ! "${WARMUP}" =~ ^[0-9]+$ ]]; then
  log "ERROR: WARMUP must be a non-negative integer, got '${WARMUP}'"
  exit 1
fi

if ! conn_err=$(mysql_exec "SELECT 1;" 2>&1); then
  log "ERROR: cannot connect seekdb（命令：${MYSQL}）"
  log "  实际报错：${conn_err}"
  log "  Example: MYSQL='obclient -h127.0.0.1 -P2881 -uroot -N -s --protocol=tcp --default-character-set=utf8mb4' $0"
  exit 1
fi

log "=== FTS Large Benchmark (label=${LABEL}) ==="
log "rows=${ROWS} batch=${BATCH} rounds=${ROUNDS} query_rounds=${QUERY_ROUNDS} samples=${SAMPLES}"

log "[0/5] SQL baseline ..."
run_repeated_sql_bench "select1" "${ROUNDS}" "SELECT 1;"

if [[ "${SKIP_LOAD}" != "1" ]]; then
  log "[1/5] setup raw schema ..."
  $MYSQL_VERBOSE < "${SCRIPT_DIR}/fts_large_bench_setup.sql" >/dev/null

  log "[2/5] raw bulk load ${ROWS} docs (batch=${BATCH}) ..."
  load_start_ns=$(now_ns)
  python3 "${SCRIPT_DIR}/fts_large_bench_gen.py" --rows "${ROWS}" --batch "${BATCH}" | $MYSQL_VERBOSE >/dev/null
  load_end_ns=$(now_ns)
  load_sec=$(elapsed_sec "${load_start_ns}" "${load_end_ns}")
  rps=$(awk "BEGIN {printf \"%.1f\", ${ROWS} / ${load_sec}}")
  METRICS[raw_load_sec]="${load_sec}"
  METRICS[raw_load_rows_per_sec]="${rps}"
  log "  raw load done: ${load_sec}s (${rps} rows/s)"

  loaded=$(mysql_exec "USE fts_large_bench; SELECT COUNT(*) FROM docs;" | tail -1)
  log "  table rows: ${loaded}"

  log "[3/5] build fulltext indexes ..."
  # The build is the noisiest part of the benchmark: the same commit produced
  # 2.7s and 5.7s for the same index on the platform, because a background
  # merge from the raw load competes with the build for the same cores.
  #
  # BUILD_REPEATS rebuilds the indexes that many times and keeps the best
  # result for each one. The minimum is the honest number here: a run that got
  # unlucky with the merge is not a measurement of the build code. Set it to 1
  # for a single pass.
  # This block sits at the top level of the script, so these are plain
  # assignments: `local` is only valid inside a function.
  repeats="${BUILD_REPEATS:-1}"
  build_total_sec=""
  build_start_ns=""
  build_end_ns=""
  if [[ "${repeats}" -gt 1 ]]; then
    log "  (build repeated ${repeats}x, keeping the best per index)"
  fi

  build_ik_all_best=""
  build_ik_content_best=""
  build_beng_en_best=""
  r=0
  for ((r = 1; r <= repeats; r++)); do
    # Drop only the indexes, not the data: the data load is the slow part and
    # it does not change between attempts.
    #
    # Each DROP runs on its own. The client sends a batch of statements and
    # stops at the first failure, so a batch that drops an index which does not
    # exist yet would abort before the ADD statements ever run and leave the
    # table without any fulltext index.
    if [[ "${r}" -gt 1 ]]; then
      for idx in fti_all_ik fti_content_ik fti_en_beng; do
        mysql_exec "USE fts_large_bench; ALTER TABLE docs DROP INDEX ${idx};" >/dev/null 2>&1 || true
      done
    fi

    build_start_ns=$(now_ns)
    time_mysql_command "build_ik_all" \
      "USE fts_large_bench; ALTER TABLE docs ADD FULLTEXT INDEX fti_all_ik(title, content) WITH PARSER ik;"
    time_mysql_command "build_ik_content" \
      "USE fts_large_bench; ALTER TABLE docs ADD FULLTEXT INDEX fti_content_ik(content) WITH PARSER ik;"
    time_mysql_command "build_beng_en" \
      "USE fts_large_bench; ALTER TABLE docs ADD FULLTEXT INDEX fti_en_beng(title_en, content_en) WITH PARSER beng;"
    build_end_ns=$(now_ns)

    total="$(elapsed_sec "${build_start_ns}" "${build_end_ns}")"
    if [[ -z "${build_total_sec}" ]] || awk "BEGIN {exit !(${total} < ${build_total_sec})}"; then
      build_total_sec="${total}"
    fi
    for name in build_ik_all build_ik_content build_beng_en; do
      cur="${METRICS[${name}_sec]}"
      best_var="best"
      case "${name}" in
        build_ik_all)     best_var="build_ik_all_best" ;;
        build_ik_content) best_var="build_ik_content_best" ;;
        build_beng_en)    best_var="build_beng_en_best" ;;
      esac
      best="${!best_var}"
      if [[ -z "${best}" ]] || awk "BEGIN {exit !(${cur} < ${best})}"; then
        printf -v "${best_var}" '%s' "${cur}"
      fi
    done
    log "  build round ${r}/${repeats}: total=${total}s  (ik_all=${METRICS[build_ik_all_sec]} content=${METRICS[build_ik_content_sec]} beng=${METRICS[build_beng_en_sec]})"
  done
  if [[ "${repeats}" -gt 1 ]]; then
    METRICS[build_ik_all_sec]="${build_ik_all_best}"
    METRICS[build_ik_content_sec]="${build_ik_content_best}"
    METRICS[build_beng_en_sec]="${build_beng_en_best}"
    log "  best of ${repeats}: ik_all=${build_ik_all_best} content=${build_ik_content_best} beng=${build_beng_en_best}"
  fi
  METRICS[build_total_sec]="${build_total_sec}"

  # Verify the indexes exist before scoring anything. A dropped or failed
  # index would make every later query report "Can't find FULLTEXT index" and
  # the timings would be the cost of the error, not of the code.
  missing=""
  for check_idx in fti_all_ik fti_content_ik fti_en_beng; do
    if ! mysql_exec "USE fts_large_bench; SHOW INDEX FROM docs WHERE Key_name='${check_idx}';" | grep -q "${check_idx}"; then
      missing="${missing} ${check_idx}"
    fi
  done
  if [[ -n "${missing}" ]]; then
    log "ERROR: 这些全文索引在 build 之后不存在：${missing}"
    log "       后续查询会全部失败，本轮结果无效。"
    exit 1
  fi
  log "  三个全文索引均已就绪"
else
  log "[1-3/5] SKIP_LOAD=1, reuse existing fts_large_bench.docs and indexes"
  loaded=$(mysql_exec "USE fts_large_bench; SELECT COUNT(*) FROM docs;" 2>/dev/null | tail -1 || echo "0")
  if [[ "${loaded}" == "0" ]]; then
    log "ERROR: fts_large_bench.docs is empty. Run without SKIP_LOAD first."
    exit 1
  fi
  log "  table rows: ${loaded}"
  ROWS="${loaded}"
fi

log "[4/5] TOKENIZE hot-path ..."
run_tokenize_bench "tokenize_ik" "ik" "${IK_TEXT}"
run_tokenize_bench "tokenize_beng" "beng" "${BENG_TEXT}"

log "[5/5] MATCH query ..."
run_query_bench "query_cn" \
  "SELECT COUNT(*) FROM fts_large_bench.docs WHERE MATCH(title, content) AGAINST('全文索引 分词器');"
run_query_bench "query_beng" \
  "SELECT COUNT(*) FROM fts_large_bench.docs WHERE MATCH(title_en, content_en) AGAINST('database performance audit');"
run_query_bench "query_mixed" \
  "SELECT COUNT(*) FROM fts_large_bench.docs WHERE MATCH(title, content) AGAINST('OceanBase 稳定 database');"
run_query_bench "query_limit" \
  "SELECT COUNT(*) FROM (SELECT id FROM fts_large_bench.docs WHERE MATCH(content) AGAINST('倒排索引 tokenizer') LIMIT 20) t;"

report=$(write_report)
echo "${report}"
if [[ -n "${OUTPUT}" ]]; then
  echo "${report}" >> "${OUTPUT}"
  log "Report appended to ${OUTPUT}"
fi

log ""
log "Compare before/after on these fields:"
log "  build_ik_all_sec, build_ik_content_sec, build_beng_en_sec, build_total_sec"
log "  tokenize_ik_avg_ms/stdev, tokenize_beng_avg_ms/stdev, select1_avg_ms as SQL-loop baseline"
log "  query_cn_avg_ms, query_beng_avg_ms, query_mixed_avg_ms, query_limit_avg_ms with *_hits"
