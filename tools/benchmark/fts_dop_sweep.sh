#!/usr/bin/env bash
# 在「当前这台机器」扫一遍建索引并行度，不需要重新编译。
#
# OB_FTS_BUILD_DOP 由 src/sql/resolver/ddl/ob_fts_index_builder_util.cpp 的
# fts_build_parallelism_ceiling() 从进程环境读取，覆盖默认 min(可用核数/2, 8)。
# 默认值是在旧的 8 核实例上扫出来的（1→11.850 / 2→9.747 / 3→9.940 / 4→9.437 /
# 8→10.048），换了机器就得重扫：核数、cgroup 配额、后台合并压力都变了。
#
# 用法（在实例上执行）：
#   bash tools/benchmark/fts_dop_sweep.sh
#   DOPS="4 6 8" BUILD_REPEATS=3 bash tools/benchmark/fts_dop_sweep.sh
#
# 每个 DOP 都走 comp_recover.sh bench，它会起一个干净实例（rm -rf /tmp/sdb）、
# 灌 20000 篇、建三个索引，所以结果不受上一轮残留索引和后台合并干扰。

set -uo pipefail

SRC_DIR="${SRC_DIR:-/tmp/seekdb}"
LOG_DIR="${LOG_DIR:-/tmp/recover}"
DOPS="${DOPS:-1 2 3 4 6 8}"
BUILD_REPEATS="${BUILD_REPEATS:-2}"

# 主逻辑必须包在函数里：check_bench_scripts.py 会把函数体剥掉后真跑一遍顶层，
# 顶层留着裸 for 循环的话，检查器会真的去跑 6 轮 bench，30 秒超时后判定"脚本卡住"。
main() {
mkdir -p "${LOG_DIR}"
summary="${LOG_DIR}/dop_sweep.txt"
: > "${summary}"

echo "DOP 扫描：dops=[${DOPS}]  BUILD_REPEATS=${BUILD_REPEATS}" | tee -a "${summary}"
echo "机器: online=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo '?')  $(uname -r)" | tee -a "${summary}"
echo | tee -a "${summary}"

for dop in ${DOPS}; do
  label="dop${dop}"
  out="${LOG_DIR}/dop_sweep_${label}.stdout"
  report="${LOG_DIR}/fts_${label}.txt"

  echo "--- DOP=${dop} ---"
  if ! OB_FTS_BUILD_DOP="${dop}" BUILD_REPEATS="${BUILD_REPEATS}" \
        bash "${SRC_DIR}/tools/comp_recover.sh" bench "${label}" > "${out}" 2>&1; then
    echo "  ✗ 失败，看 ${out}" | tee -a "${summary}"
    tail -20 "${out}"
    continue
  fi

  if [ ! -f "${report}" ]; then
    echo "  ✗ 报告没落盘：${report}" | tee -a "${summary}"
    continue
  fi

  # 校验覆盖真的生效：observer 会打一条 "fts build parallelism ceiling"。
  applied="$(grep -rh "fts build parallelism ceiling" /tmp/sdb/log/ /tmp/sdb/server.log 2>/dev/null \
             | tail -1 | sed 's/.*] //')"
  echo "  ceiling: ${applied:-（没找到日志，需人工确认）}"

  total="$(grep -m1 '^build_total_sec:' "${report}" | awk '{print $2}')"
  cores="$(grep -m1 '^build_cores:' "${report}" | sed 's/^build_cores:[[:space:]]*//')"
  cpu="$(grep -m1 '^build_cpu_sec:' "${report}" | sed 's/^build_cpu_sec:[[:space:]]*//')"
  load="$(grep -m1 '^raw_load_sec:' "${report}" | awk '{print $2}')"
  # 卡顿的判据不是三项之间的 max/min：ik_all 本来就比 beng 慢 ~2.2 倍，
  # 那是固有形态。改看归一化后的形状（content=1），正常约 all=1.29 / beng=0.55；
  # 某一项被拖住时对应比值会翻倍。
  shape="$(awk '/^build_ik_all_sec:/{a=$2} /^build_ik_content_sec:/{c=$2} /^build_beng_en_sec:/{b=$2} \
                END{if(c>0) printf "all=%.2f,beng=%.2f", a/c, b/c; else print "N/A"}' "${report}")"
  printf 'DOP=%s build_total=%ss cpu=%s cores=%s shape=%s raw_load=%ss\n' \
         "${dop}" "${total:-N/A}" "${cpu:-N/A}" "${cores:-N/A}" \
         "${shape:-N/A}" "${load:-N/A}" | tee -a "${summary}"

  # 哨兵：灌数被后台合并干扰的轮次不能用来比 DOP。
  awk -v v="${load:-0}" 'BEGIN {
    if (v + 0 > 1.0) printf "  ! raw_load=%ss 偏高，这一轮作废\n", v }'
done

echo | tee -a "${summary}"
echo "汇总（按 build_total 升序；shape 正常约 all=1.29,beng=0.55，明显偏离即单相卡顿）" | tee -a "${summary}"
grep '^DOP=' "${summary}" | sort -t= -k3 -n | head -10 | tee -a "${summary}"
echo "完整结果：${summary}"
}

main "$@"
