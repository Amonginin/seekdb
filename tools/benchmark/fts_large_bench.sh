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
PROFILE_BUILD="${PROFILE_BUILD:-0}"
PROBE_THREADS="${PROBE_THREADS:-0}"
# DDL 并行度覆盖：会话级变量 _force_parallel_ddl_dop。
# 默认值 1，而 DDL 规划器按「hint > 该变量 > 表 dop > 1」取值
# （ob_ddl_resolver.cpp:7952 calc_ddl_parallelism）。docs 是单分区表、建表没写
# PARALLEL，所以理论并行度就是 1；这个变量是唯一不需要改代码就能把它抬起来的开关。
# >1 时本脚本在每条 SQL 前注入 SET SESSION，从而对紧随其后的 ALTER 生效。
DDL_DOP="${DDL_DOP:-0}"
DDL_DOP_SQL=""
OUTPUT="${OUTPUT:-}"

IK_TEXT="OceanBase是一款非常稳定的数据库，全文索引的分词器热路径优化包括解析器实例复用、停用词全局单例、内存池化数据结构等技术，在大量文档索引和查询场景下可以显著降低 CPU 开销。"
BENG_TEXT="The quick brown fox jumps over the lazy dog. Full-text search indexing requires efficient tokenization on the hot path."

declare -A METRICS
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/fts_large_bench.XXXXXX")"
# observer 进程 pid：CPU 秒与并行度指标要用。取不到就只报耗时，不影响压测本身。
OBSERVER_PID=""
if command -v pgrep >/dev/null 2>&1; then
  OBSERVER_PID="$(pgrep -f 'seekdb --base-dir' 2>/dev/null | head -1)"
elif command -v pidof >/dev/null 2>&1; then
  OBSERVER_PID="$(pidof seekdb 2>/dev/null | awk '{print $1}')"
fi

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

mysql_exec() { $MYSQL -e "SET NAMES utf8mb4; SET ob_query_timeout = 100000000000; SET ob_trx_timeout = 100000000000; ${DDL_DOP_SQL}$1"; }

# 先试一次 SET：不同版本/发行版未必有这个变量，失败就明确说无效，
# 而不是让后面每条 ALTER 都因为一句 SET 报错而整体失败。
setup_ddl_dop() {
  if [[ ! "${DDL_DOP}" =~ ^[0-9]+$ ]] || [[ "${DDL_DOP}" -le 1 ]]; then
    return 0
  fi
  if $MYSQL -e "SET SESSION _force_parallel_ddl_dop=${DDL_DOP};" >/dev/null 2>&1; then
    DDL_DOP_SQL="SET SESSION _force_parallel_ddl_dop=${DDL_DOP}; "
    log "DDL 并行度覆盖：_force_parallel_ddl_dop=${DDL_DOP}（会话级，对后续 ALTER 生效）"
  else
    log "WARN: 该版本不支持 _force_parallel_ddl_dop，DDL_DOP=${DDL_DOP} 被忽略"
  fi
}

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

# observer 进程累计 CPU 时间（单位：时钟滴答）。utime+stime 对全部线程求和。
# 字段偏移：/proc/<pid>/task/<tid>/stat 的第 2 个字段 comm 可能含空格和括号，
# 所以先删掉到最后一个 ') ' 为止的前缀，剩下的字段就从 state 开始，utime 变成第 12 个。
# 全部线程的 CPU 滴答。awk 一次吃掉所有文件：几百个线程时逐文件 fork awk
# 会把采样开销做到秒级。
proc_cpu_ticks() {
  local pid="$1"
  local files=(/proc/"${pid}"/task/*/stat)
  awk '{ sub(/^.*\) /, ""); s += $12 + $13 } END { printf "%.0f", s + 0 }' "${files[@]}" 2>/dev/null
}

# 建索引线程自己的 CPU 不能用「ALTER 前后取差值」算：DDL 线程池在 ALTER 期间
# 创建、结束后销毁，两个采样点都看不到它们，差值恒为 0。
# 正确做法是在 ALTER 期间按 tid 跟踪，见 probe_report()。


proc_thread_count() {
  local pid="$1"
  ls -1 /proc/"${pid}"/task 2>/dev/null | wc -l
}

# 一次 ALTER 的耗时之外，再记两个数：
#   build_xxx_cpu_sec = 这一次 ALTER 烧掉多少 CPU 秒
#   build_xxx_cores   = cpu_sec / wall，也就是「实际吃了几核」
# 这两个数把「并行到底有没有生效」变成可直接读的指标，不需要任何 profiler：
#   cores ≈ 1   → 建索引其实是单线程，DOP 参数是空转，要往并行链路查
#   cores ≈ DOP → 真的并行，瓶颈在单核效率，要往热函数查
time_mysql_command() {
  local name="$1" sql="$2"
  local start_ns end_ns total_sec
  local ticks0="" ticks1="" cpu_sec="" cores="" hz
  hz="$(getconf CLK_TCK 2>/dev/null || echo 100)"
  if [ -n "${OBSERVER_PID}" ] && [ -d "/proc/${OBSERVER_PID}" ]; then
    ticks0="$(proc_cpu_ticks "${OBSERVER_PID}")"
  fi
  start_ns=$(now_ns)
  mysql_exec "${sql}" >/dev/null
  end_ns=$(now_ns)
  if [ -n "${ticks0}" ] && [ -d "/proc/${OBSERVER_PID}" ]; then
    ticks1="$(proc_cpu_ticks "${OBSERVER_PID}")"
    cpu_sec="$(awk -v a="${ticks0}" -v b="${ticks1}" -v hz="${hz}" 'BEGIN { printf "%.3f", (b - a) / hz }')"
  fi
  total_sec=$(elapsed_sec "${start_ns}" "${end_ns}")
  METRICS["${name}_sec"]="${total_sec}"
  if [ -n "${cpu_sec}" ]; then
    cores="$(awk -v c="${cpu_sec}" -v t="${total_sec}" 'BEGIN { if (t > 0) printf "%.2f", c / t; else printf "0" }')"
    METRICS["${name}_cpu_sec"]="${cpu_sec}"
    METRICS["${name}_cores"]="${cores}"
    log "  ${name}: ${total_sec}s  cpu=${cpu_sec}s cores=${cores}"
  else
    log "  ${name}: ${total_sec}s"
  fi
}

# ---------------------------------------------------------------- thread probe
# PROBE_THREADS=1：每次 ALTER 期间每 200ms 采一次 observer 的线程状态。
#
# 为什么需要它：Amdahl 反推 build 有 80.8% 是串行的（DOP 1→4 只快 1.169 倍）。
# perf 在本实例被封死（perf_event_open 被容器拒绝），gdb 会把 build 拖慢 80 倍，
# 而 /proc 不需要 profiler。
#
# 2026-11-14 实测三条教训，直接决定了这个实现：
#   1. /proc/<tid>/wchan 在这台内核恒为 "0"，改读 /proc/<tid>/syscall 的调用号；
#   2. DDL_* 线程单次 ALTER 只烧 0.4s CPU，PX_G* 才是主力（约 8s / 9.9s）；
#   3. 线程池里大量空闲线程会把统计淹没（它们永远在 futex 上等任务），
#      所以 state/syscall 只统计「CPU 比上一个采样点有增长」的活跃样本。
#
# 输出按 tid 展开。PX_G0 可能对应多个 tid（同一 group 的多个 worker），
# 只按名字聚合看不出开了几个 worker —— 而 worker 数正是并行度的硬上限。
PROBE_JOB=""

probe_syscall_name() {
  case "$1" in
    0) echo read ;; 1) echo write ;; 2) echo open ;; 3) echo close ;;
    7) echo poll ;; 17) echo pread64 ;; 18) echo pwrite64 ;;
    19) echo readv ;; 20) echo writev ;; 23) echo select ;;
    35) echo nanosleep ;; 56) echo clone ;; 202) echo futex ;;
    230) echo clock_nanosleep ;; 232) echo epoll_wait ;;
    271) echo pwritev ;; 281) echo epoll_pwait ;;
    -1|running) echo running ;;
    *) echo "nr=$1" ;;
  esac
}

probe_start() {
  local out="$1"
  : > "${out}"
  rm -f "${TMP_DIR}/probe.stop"
  (
    seq=0
    while [[ ! -f "${TMP_DIR}/probe.stop" ]]; do
      seq=$((seq + 1))
      # 一次 awk 读完所有线程的 stat：几百个线程逐个 fork awk 会把采样开销做到秒级。
      awk '{
        line = $0
        tid = line
        sub(/ .*/, "", tid)
        sub(/^[0-9]+ \(/, "", line)
        idx = index(line, ") ")
        if (idx <= 0) { next }
        comm = substr(line, 1, idx - 1)
        rest = substr(line, idx + 2)
        n = split(rest, f, " ")
        if (n < 13) { next }
        printf "%s|%s|%s|%d\n", tid, comm, f[1], f[12] + f[13]
      }' /proc/"${OBSERVER_PID}"/task/*/stat 2>/dev/null > "${TMP_DIR}/snap.txt" || true

      while IFS='|' read -r tid comm state cpu; do
        case "${comm}" in
          PX_*|DDL_*) ;;
          *) continue ;;
        esac
        nr="?"
        read -r nr _rest < "/proc/${OBSERVER_PID}/task/${tid}/syscall" 2>/dev/null || nr="?"
        printf '%s|%s|%s|%s|%s|%s\n' "${seq}" "${tid}" "${comm}" "${state}" "${cpu}" "$(probe_syscall_name "${nr}")"
      done < "${TMP_DIR}/snap.txt"
      sleep 0.2
    done
  ) >> "${out}" 2>/dev/null &
  PROBE_JOB=$!
}

probe_stop() {
  if [[ -n "${PROBE_JOB}" ]]; then
    : > "${TMP_DIR}/probe.stop"
    # 采样循环被 stop 文件打断时退出码非 0，job 也可能已被回收；set -e 下必须吞掉。
    wait "${PROBE_JOB}" 2>/dev/null || true
    PROBE_JOB=""
  fi
}

probe_report() {
  local name="$1" out="$2" wall="${3:-0}"
  if [[ ! -s "${out}" ]]; then
    log "  [probe] ${name}: 没采到线程"
    return 0
  fi
  local hz samples
  hz="$(getconf CLK_TCK 2>/dev/null || echo 100)"
  samples="$(awk -F'|' 'NF>=6 { if ($1+0 > m) m = $1+0 } END { print m+0 }' "${out}")"
  log "  [probe] ${name}: 采样点=${samples}  窗口=${wall}s"

  # ① 每个 tid 的窗口内 CPU = 末次 - 首次。注意不能用峰值，那是进程启动以来的累计值。
  awk -F'|' -v hz="${hz}" -v wall="${wall}" '
    NF >= 6 {
      tid = $2; comm = $3; cpu = $5 + 0
      if (!(tid in first)) { first[tid] = cpu; commof[tid] = comm; tids[++n] = tid }
      last[tid] = cpu
    }
    END {
      pxn = 0; pxsum = 0; ddn = 0; ddsum = 0
      for (i = 1; i <= n; i++) {
        t = tids[i]; d = last[t] - first[t]
        if (commof[t] ~ /^PX_/) { pxn++; pxsum += d; printf "PXROW\t%s\t%s\t%.2f\n", commof[t], t, d / hz }
        else if (d > 0) { ddn++; ddsum += d }
      }
      printf "PXSUM\t%d\t%.2f\t%.2f\n", pxn, pxsum / hz, (wall > 0 ? pxsum / hz / wall : 0)
      printf "DDLSUM\t%d\t%.2f\n", ddn, ddsum / hz
    }' "${out}" > "${TMP_DIR}/probe_p1.txt"

  log "    PX worker：线程数=$(awk -F'\t' '$1=="PXSUM"{print $2}' "${TMP_DIR}/probe_p1.txt")  CPU=$(awk -F'\t' '$1=="PXSUM"{print $3}' "${TMP_DIR}/probe_p1.txt")s  有效并行度=$(awk -F'\t' '$1=="PXSUM"{print $4}' "${TMP_DIR}/probe_p1.txt")"
  awk -F'\t' '$1=="PXROW" && $4+0 > 0' "${TMP_DIR}/probe_p1.txt" \
    | sort -t"	" -k4,4gr \
    | awk -F'\t' 'NR<=8 { printf "      %s tid=%-8s cpu=%ss\n", $2, $3, $4 }'
  log "    DDL 活跃线程：数=$(awk -F'\t' '$1=="DDLSUM"{print $2}' "${TMP_DIR}/probe_p1.txt")  CPU=$(awk -F'\t' '$1=="DDLSUM"{print $3}' "${TMP_DIR}/probe_p1.txt")s"

  # ② 只统计活跃样本：CPU 比上一个采样点有增长，说明这个间隔内确实在干活。
  awk -F'|' '
    NF >= 6 {
      tid = $2; comm = $3; state = $4; cpu = $5 + 0; sc = $6
      grp = (comm ~ /^PX_/) ? "PX" : "DDL"
      if ((tid in prev) && cpu > prev[tid]) { st[grp "|" state]++; scn[grp "|" sc]++; cnt[grp]++ }
      prev[tid] = cpu
    }
    END {
      split("PX DDL", g, " ")
      for (i = 1; i <= 2; i++) {
        k = g[i]
        if (cnt[k] + 0 == 0) continue
        line = "    " k " 活跃样本 state:"
        for (x in st) { split(x, a, "|"); if (a[1] == k) line = line sprintf(" %s=%.0f%%", a[2], 100 * st[x] / cnt[k]) }
        print line
        line = "    " k " 活跃样本 syscall:"
        for (x in scn) { split(x, a, "|"); if (a[1] == k && scn[x] * 100 / cnt[k] >= 5) line = line sprintf(" %s=%.0f%%", a[2], 100 * scn[x] / cnt[k]) }
        print line
      }
    }' "${out}"
}

# ---------------------------------------------------------------- build profiling# ---------------------------------------------------------------- build profiling# ---------------------------------------------------------------- build profiling
# PROFILE_BUILD=1：在三次 ALTER 期间给 observer 采样，回答「build 的 4~6s/索引 到底花在哪个函数」。
#
# 为什么需要它：build 占评分 coeff 的 52%，而我们只知道「写行 300K 行/秒」这一个间接数字，
# 分不清是分词、组行、还是插入路径的行开销。靠猜已经错过两次（分词不是瓶颈、relevance 是承重墙）。
#
# 2026-11-12 实测教训：gdb 采样会 ptrace 停住整个进程，build 从 12s 变成 992s（80 倍），
# 采到的全是空闲线程的 futex / cond_wait，彻底不可用。所以 gdb 只在显式 PROFILE_GDB=1
# 时启用，且限制在 20 个样本；默认要求 perf，没有就立刻失败并给安装命令。
PROFILE_KIND=""
PROFILE_DATA=""
PROFILE_JOB=""
PROFILE_PID=""
PERF_BIN=""
PROFILE_GDB="${PROFILE_GDB:-0}"
PROFILE_EVENT="${PROFILE_EVENT:-cpu-clock}"

# /usr/bin/perf 是个 wrapper，会按 uname -r 去 /usr/lib/linux-tools/<ver>/ 找二进制。
# 魔搭实例跑的是自定义内核 5.10.134-...lifsea8，Ubuntu 仓库里没有对应包，
# wrapper 直接报 "perf not found for kernel"。但 5.15 的那个二进制对
# cpu-clock 这类软件事件通常照样能跑，所以别只看 command -v，要真采一次。
resolve_perf_bin() {
  local cand data
  data="${TMP_DIR}/perf_selftest.data"
  for cand in "$(command -v perf 2>/dev/null)" /usr/lib/linux-tools/*/perf; do
    [ -n "${cand}" ] && [ -x "${cand}" ] || continue
    rm -f "${data}"
    if "${cand}" record -e "${PROFILE_EVENT}" -F 99 -o "${data}" -- sleep 0.3 >/dev/null 2>&1 \
       && [ -s "${data}" ]; then
      PERF_BIN="${cand}"
      return 0
    fi
  done
  return 1
}

profile_precheck() {
  PROFILE_PID="$(pgrep -f 'seekdb --base-dir' | head -1)"
  if [[ -z "${PROFILE_PID}" ]]; then
    log "ERROR: PROFILE_BUILD=1 但找不到 observer 进程（seekdb --base-dir ...）"
    exit 1
  fi
  if resolve_perf_bin; then
    PROFILE_KIND="perf"
    log "  [profile] 可用 perf：${PERF_BIN}（事件 ${PROFILE_EVENT}）"
    return 0
  fi
  if [[ "${PROFILE_GDB}" == "1" ]] && command -v gdb >/dev/null 2>&1; then
    PROFILE_KIND="gdb"
    log "  [profile] 警告：gdb 采样会把 build 拖慢两个数量级，这一轮的耗时数字不可用（只取热点）"
    return 0
  fi
  log "ERROR: perf 装了但无法采样（内核 5.10.134 自定义 + perf_event_paranoid 只读，容器里常见）。"
  log "  已试过：/usr/bin/perf wrapper 与 /usr/lib/linux-tools/*/perf 的 cpu-clock 自测全部失败。"
  log "  这说明本实例不能用 perf_event_open。可选："
  log "    1) 换一台允许 perf 的实例；"
  log "    2) 用不含 profiler 的 build_cores / build_cpu_sec 指标继续定位（推荐）；"
  log "    3) PROFILE_GDB=1 退化为 20 个样本的粗采样（耗时数字会失真）。"
  exit 1
}

profile_start() {
  PROFILE_DATA="${TMP_DIR}/profile_samples.txt"
  : > "${PROFILE_DATA}"
  case "${PROFILE_KIND}" in
    perf)
      rm -f "${PROFILE_DATA}"
      # 不带子命令时 perf 会一直采样到 SIGINT，正好覆盖三次 ALTER 的窗口。
      "${PERF_BIN}" record -e "${PROFILE_EVENT}" -F 499 -g -p "${PROFILE_PID}" -o "${PROFILE_DATA}" >/dev/null 2>&1 &
      PROFILE_JOB=$!
      sleep 1
      if ! kill -0 "${PROFILE_JOB}" 2>/dev/null; then
        log "  [profile] perf 启动失败，多半是 perf_event_paranoid 限制：sysctl -w kernel.perf_event_paranoid=-1"
        PROFILE_KIND=""
        return 0
      fi
      log "  [profile] perf 采样中 pid=${PROFILE_PID} 频率=499Hz 带调用图"
      ;;
    gdb)
      (
        for _ in $(seq 1 20); do
          gdb -p "${PROFILE_PID}" -batch -ex "thread apply all bt 8" 2>/dev/null >> "${PROFILE_DATA}"
          echo "----SAMPLE----" >> "${PROFILE_DATA}"
          sleep 0.5
        done
      ) &
      PROFILE_JOB=$!
      log "  [profile] gdb 采样中 pid=${PROFILE_PID}（最多 20 个样本）"
      ;;
  esac
}

profile_stop() {
  case "${PROFILE_KIND}" in
    perf)
      kill -INT "${PROFILE_JOB}" 2>/dev/null
      wait "${PROFILE_JOB}" 2>/dev/null
      log "  [profile] 采样结束 -> ${PROFILE_DATA}"
      ;;
    gdb)
      # 采样进程自己跑完 20 个样本就退出；这里只等它，不强杀（强杀会留下半截栈）。
      wait "${PROFILE_JOB}" 2>/dev/null
      log "  [profile] 采样结束 -> ${PROFILE_DATA}"
      ;;
  esac
}

profile_report() {
  case "${PROFILE_KIND}" in
    perf)
      if [[ ! -s "${PROFILE_DATA}" ]]; then
        log "  [profile] 没采到样本"
        return 0
      fi
      log "  ---- perf 平面报告（叶子函数自身 CPU 占比）----"
      "${PERF_BIN}" report --stdio -i "${PROFILE_DATA}" --sort symbol --percent-limit 1 2>/dev/null | sed -n '1,45p'
      log "  ---- perf 按 dso+symbol（区分 observer 自身 / libc / 第三方）----"
      "${PERF_BIN}" report --stdio -i "${PROFILE_DATA}" --sort dso,symbol --percent-limit 1 2>/dev/null | sed -n '1,30p'
      ;;
    gdb)
      if [[ ! -s "${PROFILE_DATA}" ]]; then
        log "  [profile] 没采到样本"
        return 0
      fi
      log "  ---- gdb 采样热点（已剔除 futex/cond_wait/nanosleep 等空闲帧）----"
      grep -oE ' in [A-Za-z_][A-Za-z0-9_:<>~]*' "${PROFILE_DATA}" \
        | sed 's/^ in //' \
        | grep -vE '^(futex|__pthread_cond_wait_common|__GI___nanosleep|__GI___clock_nanosleep|usleep|clone|start_thread|__GI___poll|epoll_wait|__select|select)$' \
        | grep -vE 'ObLightyCond|ObThreadCond::wait|ObSimpleThreadPoolBase|ObAsyncTaskQueue::pop|thread_cond_wait|SimpleCond::wait|Thread::__th_start|ObReentrantThread::blocking_run' \
        | sort | uniq -c | sort -rn | head -30
      ;;
    *)
      log "  [profile] 本轮没有采样数据"
      ;;
  esac
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
    echo "ddl_dop:                ${DDL_DOP}"
    echo "----------------------------------------"
    echo "select1_avg_ms:         ${METRICS[select1_avg_ms]:-N/A}"
    echo "select1_stdev_ms:       ${METRICS[select1_stdev_ms]:-N/A}"
    echo "raw_load_sec:           ${METRICS[raw_load_sec]:-N/A}"
    echo "raw_load_rows_per_sec:  ${METRICS[raw_load_rows_per_sec]:-N/A}"
    echo "build_ik_all_sec:       ${METRICS[build_ik_all_sec]:-N/A}"
    echo "build_ik_content_sec:   ${METRICS[build_ik_content_sec]:-N/A}"
    echo "build_beng_en_sec:      ${METRICS[build_beng_en_sec]:-N/A}"
    echo "build_total_sec:        ${METRICS[build_total_sec]:-N/A}"
    echo "build_cpu_sec:          ik_all=${METRICS[build_ik_all_cpu_sec]:-N/A} content=${METRICS[build_ik_content_cpu_sec]:-N/A} beng=${METRICS[build_beng_en_cpu_sec]:-N/A}"
    echo "build_cores:            ik_all=${METRICS[build_ik_all_cores]:-N/A} content=${METRICS[build_ik_content_cores]:-N/A} beng=${METRICS[build_beng_en_cores]:-N/A}"
    # build_cores 是整个 observer 进程的（含后台合并/日志），不等于建索引的并行度。
    # 建索引线程自己的 CPU 见 PROBE_THREADS=1 的报告。
    # 墙钟受调度和后台合并影响，同一二进制两轮能有 3% 以上的差；CPU 秒只反映真实
    # 工作量，是判断「每行开销」类改动的可靠指标，所以单独给一个合计数。
    echo "build_cpu_total_sec:    ${METRICS[build_cpu_total_sec]:-$(awk -v a="${METRICS[build_ik_all_cpu_sec]:-0}" \
                                          -v b="${METRICS[build_ik_content_cpu_sec]:-0}" \
                                          -v c="${METRICS[build_beng_en_cpu_sec]:-0}" \
                                          'BEGIN { printf "%.3f", a + b + c }')}"
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

setup_ddl_dop

log "[0/5] SQL baseline ..."
run_repeated_sql_bench "select1" "${ROUNDS}" "SELECT 1;"

# 采样模式的检查放在最前面：缺 perf 时让人 10 秒后就知道，
# 而不是灌完 20000 篇、进了 build 才发现。
if [[ "${PROFILE_BUILD}" == "1" ]]; then
  profile_precheck
fi

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

    # 采样器必须在计时窗口之外启停：perf 启动要 sleep 1，收尾要 flush，
    # 这两段若落在窗口里会把 build_total 抬高，profiling 那轮的数字就不能用了。
    if [[ "${PROFILE_BUILD}" == "1" && "${r}" -eq 1 ]]; then
      profile_start
    fi
    build_start_ns=$(now_ns)
    if [[ "${PROBE_THREADS}" == "1" ]]; then probe_start "${TMP_DIR}/probe_ik_all.txt"; fi
    time_mysql_command "build_ik_all" \
      "USE fts_large_bench; ALTER TABLE docs ADD FULLTEXT INDEX fti_all_ik(title, content) WITH PARSER ik;"
    if [[ "${PROBE_THREADS}" == "1" ]]; then probe_stop; probe_report "build_ik_all" "${TMP_DIR}/probe_ik_all.txt" "${METRICS[build_ik_all_sec]:-0}"; fi

    if [[ "${PROBE_THREADS}" == "1" ]]; then probe_start "${TMP_DIR}/probe_content.txt"; fi
    time_mysql_command "build_ik_content" \
      "USE fts_large_bench; ALTER TABLE docs ADD FULLTEXT INDEX fti_content_ik(content) WITH PARSER ik;"
    if [[ "${PROBE_THREADS}" == "1" ]]; then probe_stop; probe_report "build_ik_content" "${TMP_DIR}/probe_content.txt" "${METRICS[build_ik_content_sec]:-0}"; fi

    if [[ "${PROBE_THREADS}" == "1" ]]; then probe_start "${TMP_DIR}/probe_beng.txt"; fi
    time_mysql_command "build_beng_en" \
      "USE fts_large_bench; ALTER TABLE docs ADD FULLTEXT INDEX fti_en_beng(title_en, content_en) WITH PARSER beng;"
    if [[ "${PROBE_THREADS}" == "1" ]]; then probe_stop; probe_report "build_beng_en" "${TMP_DIR}/probe_beng.txt" "${METRICS[build_beng_en_sec]:-0}"; fi
    build_end_ns=$(now_ns)
    if [[ "${PROFILE_BUILD}" == "1" && "${r}" -eq 1 ]]; then
      profile_stop
    fi

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
    # build_cpu_sec 每次 ALTER 都会被覆盖写，所以到最后一轮只剩最后一轮的值，
    # 和 wall 的 min-of-N 口径不一致。这里按「轮」把三项 CPU 秒加起来取最小，
    # 让 CPU 与 wall 是同一轮的数（CPU 比 wall 稳，同二进制两轮 wall 差 3.2%、
    # CPU 只差 0.3%，判断每行开销类改动只能用它）。
    round_cpu=""
    for cname in build_ik_all build_ik_content build_beng_en; do
      cval="${METRICS[${cname}_cpu_sec]:-}"
      if [[ -n "${cval}" ]]; then
        round_cpu=$(awk -v a="${round_cpu:-0}" -v b="${cval}" 'BEGIN { printf "%.3f", a + b }')
      fi
    done
    if [[ -n "${round_cpu}" ]]; then
      if [[ -z "${build_cpu_total_best:-}" ]] || awk "BEGIN {exit !(${round_cpu} < ${build_cpu_total_best})}"; then
        build_cpu_total_best="${round_cpu}"
      fi
    fi
    log "  build round ${r}/${repeats}: total=${total}s cpu=${round_cpu:-N/A}s  (ik_all=${METRICS[build_ik_all_sec]} content=${METRICS[build_ik_content_sec]} beng=${METRICS[build_beng_en_sec]})"
  done
  if [[ "${repeats}" -gt 1 ]]; then
    METRICS[build_ik_all_sec]="${build_ik_all_best}"
    METRICS[build_ik_content_sec]="${build_ik_content_best}"
    METRICS[build_beng_en_sec]="${build_beng_en_best}"
    log "  best of ${repeats}: ik_all=${build_ik_all_best} content=${build_ik_content_best} beng=${build_beng_en_best}"
  fi
  METRICS[build_total_sec]="${build_total_sec}"
  if [[ -n "${build_cpu_total_best:-}" ]]; then
    METRICS[build_cpu_total_sec]="${build_cpu_total_best}"
  fi

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

  # 采样模式下，build 一结束就把报告打出来并退出：此时机器状态最干净，
  # 也让「编译 → 采样」一轮只占几分钟机时。报告照样落盘，供外层脚本取用。
  if [[ "${PROFILE_BUILD}" == "1" ]]; then
    profile_report
    if [[ -n "${OUTPUT}" ]]; then
      write_report >> "${OUTPUT}"
    fi
    log "PROFILE_BUILD=1：只做 build 采样，跳过分词与查询压测"
    exit 0
  fi
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
