#!/usr/bin/env bash
# 导出固定一组文本的 tokenize() 结果，用于改动前后的逐 token 对比。
#
# 用法:
#   MYSQL='obclient -h127.0.0.1 -P2881 -uroot -N -s --protocol=tcp --default-character-set=utf8mb4' \
#     bash tools/benchmark/fts_tokenize_cases.sh > /tmp/recover/before.txt
#   ...修改并重新编译...
#   MYSQL='...' bash tools/benchmark/fts_tokenize_cases.sh > /tmp/recover/after.txt
#   diff /tmp/recover/before.txt /tmp/recover/after.txt && echo IDENTICAL
#
# 覆盖：纯中文、中英混排、纯英文(ik/beng)、标点数字连接符、重复词（ALL 模式的词频）、
#       长文本（多词长命中）、DEFAULT 与 ALL 两种输出模式。

set -euo pipefail

MYSQL="${MYSQL:-mysql -h127.0.0.1 -P2881 -uroot -N -s --default-character-set=utf8mb4}"
SQL_FILE="$(mktemp "${TMPDIR:-/tmp}/fts_tokenize_cases.XXXXXX.sql")"
trap 'rm -f "${SQL_FILE}"' EXIT

cat > "${SQL_FILE}" <<'SQL'
SET NAMES utf8mb4;
SET ob_query_timeout = 100000000000;

SELECT 'cn_basic' AS label,
       tokenize('中华人民共和国成立了，我们热爱和平与发展。', 'ik') AS tokens;

SELECT 'cn_mixed' AS label,
       tokenize('OceanBase是一款非常稳定的数据库，全文索引的分词器热路径优化包括解析器实例复用、停用词全局单例、内存池化数据结构等技术，在大量文档索引和查询场景下可以显著降低 CPU 开销。', 'ik') AS tokens;

SELECT 'cn_long' AS label,
       tokenize('在数据库系统中，倒排索引是一种用于加速全文检索的数据结构，它把文档中的每个词项映射到包含该词项的文档编号列表，查询时只需取出相关词项的列表并求交集即可。', 'ik') AS tokens;

SELECT 'en_with_ik' AS label,
       tokenize('The quick brown fox jumps over the lazy dog. Full-text search indexing requires efficient tokenization on the hot path.', 'ik') AS tokens;

SELECT 'en_with_beng' AS label,
       tokenize('The quick brown fox jumps over the lazy dog. Full-text search indexing requires efficient tokenization on the hot path.', 'beng') AS tokens;

SELECT 'punct_digit' AS label,
       tokenize('API 3.14 版本于 2026-11-11 发布，支持 a-b_c@d.com 与 100% 兼容。', 'ik') AS tokens;

SELECT 'repeat_words' AS label,
       tokenize('数据库数据库索引索引分词 分词 分词', 'ik') AS tokens;

SELECT 'cn_all_mode' AS label,
       tokenize('中华人民共和国成立了，我们热爱和平与发展。数据库数据库索引索引分词', 'ik', '[{"output_mode":"all"}]') AS tokens;

SELECT 'mixed_all_mode' AS label,
       tokenize('OceanBase是一款非常稳定的数据库，全文索引的分词器热路径优化。The quick brown fox jumps over the lazy dog.', 'ik', '[{"output_mode":"all"}]') AS tokens;

SELECT 'en_all_mode' AS label,
       tokenize('The quick brown fox jumps over the lazy dog. Full-text search indexing requires efficient tokenization on the hot path.', 'beng', '[{"output_mode":"all"}]') AS tokens;
SQL

"${MYSQL}" --version >/dev/null 2>&1 || {
  echo "ERROR: cannot run the client: ${MYSQL}" >&2
  exit 1
}

$MYSQL < "${SQL_FILE}"
