#!/usr/bin/env bash
# 批量 tokenize 压测：把「单条语句 ~35µs 的固定开销」摊到整表上，
# 用来做分词器改动的 A/B。单条 SELECT tokenize(...) 的噪声约 ±8%，
# 这里一跑 2 万行，总时长远超语句开销，噪声可压到 1% 以内。
#
# 用法:
#   LABEL=before ./tools/benchmark/fts_tokenize_batch.sh
#   LABEL=after  SKIP_LOAD=1 ./tools/benchmark/fts_tokenize_batch.sh
#   OUTPUT=/tmp/recover/fts_tok.txt LABEL=after ./tools/benchmark/fts_tokenize_batch.sh
#
# 环境变量:
#   MYSQL        客户端命令（默认 mysql -h127.0.0.1 -P2881 -uroot -N -s --default-character-set=utf8mb4）
#   ROWS         文档行数（默认 20000，与 fts_large_bench 一致）
#   ROUNDS       整表扫描重复次数（默认 5）
#   PARSER       分词器（默认 ik，另测 beng）
#   SKIP_LOAD    1 表示复用已有表，跳过建表灌数
#   OUTPUT       结果追加写入的文件（可选）

set -euo pipefail

MYSQL="${MYSQL:-mysql -h127.0.0.1 -P2881 -uroot -N -s --default-character-set=utf8mb4}"
LABEL="${LABEL:-unknown}"
ROWS="${ROWS:-20000}"
ROUNDS="${ROUNDS:-5}"
PARSER="${PARSER:-ik}"
SKIP_LOAD="${SKIP_LOAD:-0}"
OUTPUT="${OUTPUT:-}"
DB="fts_tok_bench"

log() { echo "$@" >&2; }
now_ns() { date +%s%N; }

mysql_exec() { $MYSQL -e "SET NAMES utf8mb4; SET ob_query_timeout = 100000000000; $1"; }

if ! mysql_exec "SELECT 1;" >/dev/null 2>&1; then
  log "ERROR: 连不上 seekdb。示例："
  log "  MYSQL='obclient -h127.0.0.1 -P2881 -uroot -N -s --protocol=tcp --default-character-set=utf8mb4' $0"
  exit 1
fi

if [[ "${SKIP_LOAD}" != "1" ]]; then
  log "[1/3] 建表（${DB}.docs，${ROWS} 行）"
  mysql_exec "DROP DATABASE IF EXISTS ${DB}; CREATE DATABASE ${DB};"
  # 用 VARCHAR 而不是 TEXT：TEXT 是 outrow lob，tokenize() 拿到 lob 句柄会报 1210
  mysql_exec "USE ${DB}; CREATE TABLE docs (id INT PRIMARY KEY AUTO_INCREMENT, content VARCHAR(1024), content_en VARCHAR(1024)) DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;"
  python3 - "${ROWS}" > /tmp/fts_tok_load.sql <<'PY'
import sys
rows = int(sys.argv[1])
cn = ("OceanBase是一款非常稳定的数据库，全文索引的分词器热路径优化包括解析器实例复用、"
      "停用词全局单例、内存池化数据结构等技术，在大量文档索引和查询场景下可以显著降低 CPU 开销。")
en = ("The quick brown fox jumps over the lazy dog. Full-text search indexing requires "
      "efficient tokenization on the hot path.")
print("SET NAMES utf8mb4;")
batch = 500
for start in range(0, rows, batch):
    vals = ",".join(["('%s','%s')" % (cn, en)] * min(batch, rows - start))
    print("USE %s; INSERT INTO docs(content, content_en) VALUES %s;" % ("fts_tok_bench", vals))
PY
  $MYSQL < /tmp/fts_tok_load.sql > /dev/null
  loaded=$(mysql_exec "USE ${DB}; SELECT COUNT(*) FROM docs;" | tail -1)
  log "  已灌入 ${loaded} 行"
else
  log "[1/3] SKIP_LOAD=1，复用 ${DB}.docs"
  loaded=$(mysql_exec "USE ${DB}; SELECT COUNT(*) FROM docs;" | tail -1)
  ROWS="${loaded}"
fi

# 先确认语句没问题，并打出一行的分词结果备查
mysql_exec "USE ${DB}; SELECT tokenize(content, '${PARSER}') FROM docs LIMIT 1;" > /dev/null

run_round() {
  local col="$1" parser="$2"
  local sql="USE ${DB}; SELECT COUNT(*) FROM docs WHERE LENGTH(tokenize(${col}, '${parser}')) > 0;"
  local start end total
  start=$(now_ns)
  local i
  for ((i = 0; i < ROUNDS; i++)); do
    mysql_exec "${sql}" > /dev/null
  done
  end=$(now_ns)
  total=$(awk "BEGIN{printf \"%.1f\", (${end} - ${start}) / 1000000}")
  awk -v n="${ROWS}" -v r="${ROUNDS}" -v t="${total}" \
    'BEGIN{printf "%.1f %.4f", t / r, t / r / n}'
}

log "[2/3] 批量 tokenize（${ROUNDS} 轮 × ${ROWS} 行）"
read -r ik_total ik_per_row <<< "$(run_round content "${PARSER}")"
log "  ${PARSER}: ${ik_total} ms/round, ${ik_per_row} ms/row"

read -r beng_total beng_per_row <<< "$(run_round content_en beng)"
log "  beng: ${beng_total} ms/round, ${beng_per_row} ms/row"

log "[3/3] 报告"
report="$(cat <<EOF
========================================
FTS Batch Tokenize Benchmark
========================================
timestamp:              $(date '+%Y-%m-%d %H:%M:%S')
label:                  ${LABEL}
git_head:               $(git -C "$(dirname "${BASH_SOURCE[0]}")/../.." rev-parse --short HEAD 2>/dev/null || echo unknown)
rows:                   ${ROWS}
rounds:                 ${ROUNDS}
parser:                 ${PARSER}
----------------------------------------
${PARSER}_ms_per_round:        ${ik_total}
${PARSER}_ms_per_row:          ${ik_per_row}
beng_ms_per_round:      ${beng_total}
beng_ms_per_row:        ${beng_per_row}
========================================
EOF
)"
echo "${report}"
if [[ -n "${OUTPUT}" ]]; then
  echo "${report}" >> "${OUTPUT}"
  log "报告已追加到 ${OUTPUT}"
fi
