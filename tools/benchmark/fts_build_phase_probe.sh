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

# 自报版本：这几轮踩过「fetch 没拉到新提交，跑的还是旧脚本」，
# 把脚本所属的 commit 打出来，一眼能看出跑的是哪一版。
PROBE_REV="$(git -C "${SRC_DIR}" log -1 --format='%h %s' -- tools/benchmark/fts_build_phase_probe.sh 2>/dev/null || echo unknown)"

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
  # Last resort: the layout has moved between dependency bundles before, so look
  # for the binary instead of assuming one path.
  for candidate in \
    "$(find "${SRC_DIR}/deps" -maxdepth 6 -type f -name obclient 2>/dev/null | head -1)" \
    "$(find "${SRC_DIR}/deps" -maxdepth 6 -type f -name mysql 2>/dev/null | head -1)"; do
    if [ -n "${candidate}" ] && [ -x "${candidate}" ]; then
      echo "${candidate}"
      return 0
    fi
  done
  return 1
}

CLIENT="$(find_client)" || {
  echo "找不到 obclient/mysql 客户端" >&2
  echo "已尝试：" >&2
  echo "  ${SRC_DIR}/deps/3rd/u01/obclient/bin/{obclient,mysql}" >&2
  echo "  PATH 上的 obclient / mysql" >&2
  echo "  ${SRC_DIR}/deps 下任意深度名为 obclient / mysql 的文件" >&2
  echo "实际 deps 目录结构：" >&2
  ls -d "${SRC_DIR}"/deps/* 2>/dev/null | head -10 >&2
  exit 1
}
export LD_LIBRARY_PATH="$(dirname "$(dirname "${CLIENT}")")/lib:${LD_LIBRARY_PATH:-}"

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
echo "脚本版本: ${PROBE_REV}"
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
  HAVE_BENCHMARK=0
  echo "BENCHMARK 自检: 不可用，跳过单篇法，只用批量法（批量法不依赖它）"
else
  HAVE_BENCHMARK=1
  echo "BENCHMARK 自检: 可用"
fi

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

# 2) 同一篇文档 tokenize N 次（需要 BENCHMARK）
IK_MS=0
BENG_MS=0
if [ "${HAVE_BENCHMARK}" = "1" ]; then
  IK_MS=$(time_ms "SELECT BENCHMARK(${ROUNDS}, tokenize(content, 'ik')) FROM ${DB}.docs WHERE id = ${DOC_ID};")
  BENG_MS=$(time_ms "SELECT BENCHMARK(${ROUNDS}, tokenize(content_en, 'beng')) FROM ${DB}.docs WHERE id = ${DOC_ID};")
fi

# 4) 批量法：整表分词一遍，摊到每篇。
#    这比 BENCHMARK 更接近建索引时的真实开销 —— 它把每篇文档的分词都算上，
#    而且不依赖 BENCHMARK 是否存在。
BULK_BASE_MS=$(time_ms "SELECT SUM(CHAR_LENGTH(content)) FROM ${DB}.docs;")
BULK_IK_MS=$(time_ms "SELECT SUM(CHAR_LENGTH(tokenize(content, 'ik'))) FROM ${DB}.docs;")
BULK_BENG_MS=$(time_ms "SELECT SUM(CHAR_LENGTH(tokenize(content_en, 'beng'))) FROM ${DB}.docs;")

python3 - "$ROUNDS" "$BASE_MS" "$IK_MS" "$BENG_MS" "$DOC_LEN" \
         "$BULK_BASE_MS" "$BULK_IK_MS" "$BULK_BENG_MS" "${doc_count}" <<'PYEOF'
import sys
(rounds, base_ms, ik_ms, beng_ms, doc_len,
 bulk_base_ms, bulk_ik_ms, bulk_beng_ms, doc_count) = (
    int(sys.argv[1]), float(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4]), int(sys.argv[5]),
    float(sys.argv[6]), float(sys.argv[7]), float(sys.argv[8]), int(sys.argv[9]))

def per_call_ms(total_ms):
    net = max(total_ms - base_ms, 0.0)
    return net / rounds

ik = per_call_ms(ik_ms)
beng = per_call_ms(beng_ms)

print(f"空转基准（{rounds} 次 BENCHMARK 循环）: {base_ms:.0f} ms")
print(f"[单篇法] tokenize(ik)   净每次 {ik*1000:.1f} µs   (样本文档 {doc_len} 字符)")
print(f"[单篇法] tokenize(beng) 净每次 {beng*1000:.1f} µs")
print()

# 批量法：整表分词减去整表读列的基线，再摊到每篇
bulk_net = max(bulk_ik_ms - bulk_base_ms, 0.0)
bulk_per_doc_us = bulk_net / doc_count * 1000 if doc_count else 0.0
bulk_beng_net = max(bulk_beng_ms - bulk_base_ms, 0.0)
bulk_beng_per_doc_us = bulk_beng_net / doc_count * 1000 if doc_count else 0.0
print(f"读列基线（整表）: {bulk_base_ms:.0f} ms")
print(f"[批量法] tokenize(ik)   每篇 {bulk_per_doc_us:.1f} µs  （{doc_count} 篇，净 {bulk_net:.0f} ms）")
print(f"[批量法] tokenize(beng) 每篇 {bulk_beng_per_doc_us:.1f} µs")
print()

per_table_us = 161.0 / 2
print(f"参考：build_ik_content 每篇约 161µs，两张辅助表 => 每表 {per_table_us:.0f}µs")
for label, value in (("单篇法", ik * 1000), ("批量法", bulk_per_doc_us)):
    if value > 0:
        share = value / per_table_us * 100
        print(f"  {label} 分词占比: {value:.1f} / {per_table_us:.0f} = {share:.0f}%")
print()
share = (bulk_per_doc_us or ik * 1000) / per_table_us * 100
if share >= 55:
    print("=> 分词是大头，优先优化分词路径")
elif share <= 30:
    print("=> 写入是大头，优先做批量提交")
else:
    print("=> 分词与写入各占一半，两边都要动")
PYEOF
