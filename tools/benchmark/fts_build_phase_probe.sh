#!/usr/bin/env bash
# 量「分词」在一次 FTS 建索引里占多少成本。
#
# 背景：build_ik_content 要 3.2s 建 20000 篇文档的索引，每篇约 161µs。
# 这 161µs 里有两张辅助表（FTS_INDEX 与 FTS_DOC_WORD）各做一遍
# 「分词 + 写入」，所以每篇每表约 80µs = 分词 T + 写入 W。
#
# 只要量出 T，就知道该往哪优化：
#   T 占大头 → 优化分词（哈希表、collation 比较、跨表复用）
#   T 占小头 → 优化写入（批量提交、减少逐行 DML 开销）
#
# 做法是用 SQL 的 BENCHMARK() 在数据库内部循环调用 tokenize()，
# 避免客户端进程启停的开销混进来。同时测一次空转作为基准扣除。
#
# 用法（实例已在跑，且 fts_large_bench.docs 里有数据）:
#   bash tools/benchmark/fts_build_phase_probe.sh
#
set -euo pipefail

PORT="${SEEKDB_PORT:-2881}"
DB="${SEEKDB_DATABASE:-fts_large_bench}"
ROUNDS="${ROUNDS:-2000}"

SRC_DIR="${SRC_DIR:-/tmp/seekdb}"

find_client() {
  local candidate
  # The bundled client lives under deps/3rd/u01/obclient and needs its own libs,
  # which is why comp_recover.sh exports LD_LIBRARY_PATH before using it.
  for candidate in \
    "${SRC_DIR}/deps/3rd/u01/obclient/bin/obclient" \
    "${SRC_DIR}/deps/3rd/u01/obclient/bin/mysql" \
    "$(command -v obclient 2>/dev/null || true)" \
    "$(command -v mysql 2>/dev/null || true)"; do
    if [ -n "${candidate}" ] && [ -x "${candidate}" ]; then
      echo "${candidate}"
      return 0
    fi
  done
  return 1
}

CLIENT="$(find_client)" || {
  echo "找不到 obclient/mysql 客户端" >&2
  echo "已尝试：${SRC_DIR}/deps/3rd/u01/obclient/bin/{obclient,mysql} 以及 PATH" >&2
  exit 1
}
export LD_LIBRARY_PATH="${SRC_DIR}/deps/3rd/u01/obclient/lib:${LD_LIBRARY_PATH:-}"

sql() {
  "${CLIENT}" -h127.0.0.1 -P"${PORT}" -uroot -N -s --protocol=tcp \
    --default-character-set=utf8mb4 -e "$1" 2>/dev/null
}

# 用毫秒时间戳包住一次客户端调用，得到这次调用的总耗时。
time_ms() {
  local start end
  start=$(date +%s%3N)
  sql "$1" >/dev/null
  end=$(date +%s%3N)
  echo $((end - start))
}

echo "== 探针：分词成本占比 =="
echo "client: ${CLIENT}"
echo "database: ${DB}  rounds: ${ROUNDS}"
echo

# 先自检：确认 tokenize 可用、BENCHMARK 可用、表里有数据。
# 这三个任一不成立，后面的计时就没有意义，提前报出来比跑完才发现好。
check_tokenize="$(sql "SELECT tokenize('测试文本', 'ik');")"
if [ -z "${check_tokenize}" ]; then
  echo "!! tokenize('测试文本','ik') 没有返回结果，分词函数不可用或参数不对" >&2
  exit 1
fi
echo "tokenize 自检: ${check_tokenize}"

check_benchmark="$(sql "SELECT BENCHMARK(10, 1+1);")"
if [ -z "${check_benchmark}" ]; then
  echo "!! BENCHMARK() 不可用，本探针依赖它做进程内循环" >&2
  echo "   替代方案：手工把 tokenize 包进存储过程循环，或告诉我，我换一种测法" >&2
  exit 1
fi
echo "BENCHMARK 自检: 可用"

doc_count="$(sql "SELECT COUNT(*) FROM ${DB}.docs;")"
if [ -z "${doc_count}" ] || [ "${doc_count}" -lt 100 ] 2>/dev/null; then
  echo "!! ${DB}.docs 里只有 '${doc_count}' 行，先跑一次 bench 造数据" >&2
  exit 1
fi
echo "表内文档数: ${doc_count}"
echo

# 找一篇长度接近中位的文档
DOC_ID=""
DOC_LEN=""
read -r DOC_ID DOC_LEN <<< "$(sql "
  SELECT id, CHAR_LENGTH(content) FROM ${DB}.docs
  ORDER BY CHAR_LENGTH(content) LIMIT 1 OFFSET 10000;" | tr '\t' ' ')" || true

if [ -z "${DOC_ID:-}" ]; then
  echo "!! 取不到文档。先跑一次 bench，让 ${DB}.docs 有数据。" >&2
  exit 1
fi
echo "样本文档: id=${DOC_ID} 长度=${DOC_LEN} 字符"
echo

# 1) 空转基准：BENCHMARK 循环本身的成本
BASE_MS=$(time_ms "SELECT BENCHMARK(${ROUNDS}, 1+1);")

# 2) 同一篇文档 tokenize N 次
IK_MS=$(time_ms "SELECT BENCHMARK(${ROUNDS}, tokenize(content, 'ik')) FROM ${DB}.docs WHERE id = ${DOC_ID};")

# 3) 英文分词作对照
BENG_MS=$(time_ms "SELECT BENCHMARK(${ROUNDS}, tokenize(content_en, 'beng')) FROM ${DB}.docs WHERE id = ${DOC_ID};")

python3 - "$ROUNDS" "$BASE_MS" "$IK_MS" "$BENG_MS" "$DOC_LEN" <<'PYEOF'
import sys
rounds, base_ms, ik_ms, beng_ms, doc_len = (
    int(sys.argv[1]), float(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4]), int(sys.argv[5]))

def per_call_ms(total_ms):
    net = max(total_ms - base_ms, 0.0)
    return net / rounds

ik = per_call_ms(ik_ms)
beng = per_call_ms(beng_ms)

print(f"空转基准（{rounds} 次 BENCHMARK 循环）: {base_ms:.0f} ms")
print(f"tokenize(ik)   总 {ik_ms:.0f} ms  净每次 {ik*1000:.1f} µs")
print(f"tokenize(beng) 总 {beng_ms:.0f} ms  净每次 {beng*1000:.1f} µs")
print()
# build_ik_content 每篇约 161µs，覆盖两张辅助表 => 每表 80µs
per_table_us = 161.0 / 2
print(f"参考：build_ik_content 每篇约 161µs，两张辅助表 => 每表 {per_table_us:.0f}µs")
if ik > 0:
    share = ik * 1000 / per_table_us * 100
    print(f"分词占每表成本: {ik*1000:.1f} / {per_table_us:.0f} = {share:.0f}%")
    print()
    if share >= 55:
        print("=> 分词是大头，优先优化分词路径")
    elif share <= 30:
        print("=> 写入是大头，优先做批量提交")
    else:
        print("=> 分词与写入各占一半，两边都要动")
PYEOF
