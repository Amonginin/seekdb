/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef OB_TEXT_RETRIEVAL_TOKEN_ITER_H_
#define OB_TEXT_RETRIEVAL_TOKEN_ITER_H_

#include "lib/container/ob_se_array.h"
#include "lib/lock/ob_mutex.h"
#include "query/engine/expr/ob_expr.h"
#include "ob_block_max_iter.h"
#include "ob_i_sparse_retrieval_iter.h"
namespace oceanbase
{
namespace sql
{
class ObDASScanIter;
}
namespace storage
{
class ObTableScanParam;
class ObBlockMaxScoreIterator;

// A read-only cache of one inverted chain (one token of one index tablet).
// The key carries the read snapshot version, so a committed write advances the
// version and makes the stale chain unreachable. Any miss or error falls back to
// the original storage scan; the cache never becomes a correctness dependency.
struct ObFTSInvChainKey
{
  static constexpr int64_t MAX_TOKEN_LEN = 64;
  uint64_t index_id_;
  uint64_t tablet_id_;
  int64_t schema_version_;
  uint64_t snapshot_version_;
  int32_t token_len_;
  char token_[MAX_TOKEN_LEN];

  ObFTSInvChainKey() { reset(); }
  void reset()
  {
    index_id_ = 0;
    tablet_id_ = 0;
    schema_version_ = -1;
    snapshot_version_ = 0;
    token_len_ = 0;
    for (int64_t i = 0; i < MAX_TOKEN_LEN; ++i) {
      token_[i] = 0;
    }
  }
  bool operator==(const ObFTSInvChainKey &other) const
  {
    // The read snapshot advances on every autocommit statement even when no row changes,
    // so it must NOT participate in the lookup key. It stays in the struct for logging;
    // freshness is verified separately against the live inverted-index row count.
    if (index_id_ != other.index_id_ || tablet_id_ != other.tablet_id_
        || schema_version_ != other.schema_version_
        || token_len_ != other.token_len_) {
      return false;
    }
    for (int64_t i = 0; i < token_len_; ++i) {
      if (token_[i] != other.token_[i]) {
        return false;
      }
    }
    return true;
  }
};

struct ObFTSInvChain
{
  ObFTSInvChainKey key_;
  common::ObSEArray<ObDocIdExt, 16> doc_ids_;
  common::ObSEArray<double, 16> relevances_;
  int64_t token_doc_cnt_;
  double max_token_relevance_;

  ObFTSInvChain()
    : key_(), doc_ids_(), relevances_(), token_doc_cnt_(0), max_token_relevance_(-1.0) {}
};

class ObFTSInvChainCache
{
public:
  static ObFTSInvChainCache &instance();
  // Returns a pointer to an immutable, never-released chain slot. Slots live in a
  // fixed array and are filled only while the lock is held, so the pointer stays
  // valid after unlocking. No per-hit copy is needed.
  const ObFTSInvChain *get(const ObFTSInvChainKey &key);
  int put(const ObFTSInvChainKey &key,
          const common::ObSEArray<ObDocIdExt, 16> &doc_ids,
          const common::ObSEArray<double, 16> &relevances,
          const int64_t token_doc_cnt,
          const double max_token_relevance);
private:
  ObFTSInvChainCache() : chains_(), count_(0), total_docs_(0), hits_(0), misses_(0) {}
  static constexpr int64_t MAX_CHAINS = 64;
  static constexpr int64_t MAX_TOTAL_DOCS = 200000;
  lib::ObMutex lock_;
  // Fixed slots: every access is under lock_, so no caller ever holds a pointer
  // into this array and reallocation / dangling references are impossible.
  ObFTSInvChain chains_[MAX_CHAINS];
  int64_t count_;
  int64_t total_docs_;
  int64_t hits_;
  int64_t misses_;
  DISALLOW_COPY_AND_ASSIGN(ObFTSInvChainCache);
};

struct ObTextRetrievalScanIterParam
{
  ObTextRetrievalScanIterParam()
    : allocator_(nullptr),
      mem_context_(nullptr),
      inv_idx_scan_param_(nullptr),
      inv_idx_agg_param_(nullptr),
      inv_idx_scan_iter_(nullptr),
      inv_idx_agg_iter_(nullptr),
      inv_idx_agg_expr_(nullptr),
      eval_ctx_(nullptr),
      relevance_expr_(nullptr),
      inv_scan_doc_length_col_(nullptr),
      inv_scan_domain_id_col_(nullptr),
      inv_idx_agg_cache_mode_(false)
  {}

  ObArenaAllocator *allocator_;
  lib::MemoryContext mem_context_;
  ObTableScanParam *inv_idx_scan_param_;
  ObTableScanParam *inv_idx_agg_param_;
  sql::ObDASScanIter *inv_idx_scan_iter_;
  sql::ObDASScanIter *inv_idx_agg_iter_;
  sql::ObExpr *inv_idx_agg_expr_;
  sql::ObEvalCtx *eval_ctx_;
  sql::ObExpr *relevance_expr_;
  sql::ObExpr *inv_scan_doc_length_col_;
  sql::ObExpr *inv_scan_domain_id_col_;
  bool inv_idx_agg_cache_mode_;
};

class ObTextRetrievalTokenIter final : public ObISparseRetrievalDimIter
{
public:
  ObTextRetrievalTokenIter();
  virtual ~ObTextRetrievalTokenIter() {}

  int init(const ObTextRetrievalScanIterParam &iter_param);
  void reset();
  void reuse();

  // Sparse Retrieval Dimension Iter Interfaces
  virtual int get_next_row() override;
  virtual int get_next_batch(const int64_t capacity, int64_t &count) override;
  virtual int advance_to(const ObDatum &id_datum) override;
  virtual int update_scan_param(const ObString &token, common::ObArenaAllocator &allocator);
public:
  sql::ObBitVector *get_skip() { return skip_; }
  int get_token_doc_cnt(int64_t &token_doc_cnt) const;
  double get_max_token_relevance() const { return max_token_relevance_; }
  int get_chain_cache_key(ObFTSInvChainKey &key) const;
  void set_cached_doc_cnt(const int64_t doc_cnt, const double max_relevance);
  // Re-estimate the number of rows in the current token's inverted chain without
  // scanning them. Used to prove a cached chain is still fresh under a new snapshot.
  int estimate_chain_row_count(int64_t &row_cnt);
private:
  int init_calc_exprs_in_relevance_expr();
  void clear_batch_wise_evaluated_flag(const int64_t count);
  void clear_row_wise_evaluated_flag();
  int fill_token_doc_cnt();
  int fill_token_weight();
  int eval_relevance_expr();
  int batch_eval_relevance_expr(const int64_t count);
  int estimate_token_doc_cnt();
  inline bool need_inv_idx_agg() { return inv_idx_agg_iter_ != nullptr; }
  inline bool need_calc_relevance() { return inv_idx_agg_iter_ != nullptr; }
public:
  static const int64_t INV_IDX_ROWKEY_COL_CNT = 2;
private:
  lib::MemoryContext mem_context_;
  ObArenaAllocator *allocator_;
  ObTableScanParam *inv_idx_scan_param_;
  ObTableScanParam *inv_idx_agg_param_;
  sql::ObDASScanIter *inv_idx_scan_iter_;
  sql::ObDASScanIter *inv_idx_agg_iter_;
  sql::ObExpr *inv_idx_agg_expr_;
  sql::ObEvalCtx *eval_ctx_;
  sql::ObExpr *relevance_expr_;
  sql::ObExpr *inv_scan_doc_length_col_; // read from inv_scan_table
  sql::ObExpr *inv_scan_domain_id_col_; // read from inv_scan_table
  sql::ObExpr *token_weight_expr_; // fill the expr from with max_token_relevance_
  common::ObSEArray<sql::ObExpr *, 2> relevance_calc_exprs_;
  sql::ObBitVector *skip_;
  int64_t max_batch_size_;
  int64_t token_doc_cnt_;
  double max_token_relevance_;
  ObDocIdExt advance_doc_id_;
  bool token_doc_cnt_calculated_;
  bool inv_idx_agg_cache_mode_;
  bool is_inited_;
  DISALLOW_COPY_AND_ASSIGN(ObTextRetrievalTokenIter);
};

class ObTextRetrievalDaaTTokenIter final : public ObISRDaaTDimIter
{
public:
  ObTextRetrievalDaaTTokenIter();
  virtual ~ObTextRetrievalDaaTTokenIter() {}
  int init(const ObTextRetrievalScanIterParam &iter_param);
  void reuse();
  void reset();
  virtual int get_next_row() override;
  virtual int get_next_batch(const int64_t capacity, int64_t &count) override;
  virtual int advance_to(const ObDatum &id_datum) override;
  virtual int get_curr_score(double &score) const override;
  virtual int get_curr_id(const ObDatum *&id_datum) const override;
public:
  int get_token_doc_cnt(int64_t &token_doc_cnt) const { return token_iter_->get_token_doc_cnt(token_doc_cnt); }
  virtual int get_dim_max_score(double &score) override {
    int ret = OB_SUCCESS;
    score = token_iter_->get_max_token_relevance();
    if (OB_UNLIKELY(score < 0.0)) {
      // The score is not computed for this scan, so there is no upper bound to prune with. Report
      // zero, which only makes the block max scan skip pruning instead of failing the query.
      score = 0.0;
    }
    return ret;
  }
private:
  int save_relevances_and_docids();
  int save_docids();
  int try_init_chain_cache();
  void append_chain_batch();
  void commit_chain_cache();
  ObArenaAllocator *allocator_;
  ObTextRetrievalTokenIter *token_iter_;
  sql::ObEvalCtx *eval_ctx_;
  sql::ObExpr *relevance_expr_;
  sql::ObExpr *inv_scan_domain_id_col_;
  int64_t max_batch_size_;
  int64_t cur_idx_;
  int64_t count_;
  ObFixedArray<double, ObIAllocator> relevance_; // when ~ObFixedArray(), wikll destory itself
  ObFixedArray<ObDocIdExt, ObIAllocator> doc_id_;
  common::ObDatumCmpFuncType cmp_func_;
  bool cache_hit_;
  bool cache_key_inited_;
  bool cache_usable_;
  bool building_chain_;
  const ObFTSInvChain *cached_chain_;
  bool cache_writable_;
  ObFTSInvChainKey cache_key_;
  common::ObSEArray<ObDocIdExt, 16> cached_doc_ids_;
  common::ObSEArray<double, 16> cached_relevances_;
  common::ObSEArray<ObDocIdExt, 16> building_doc_ids_;
  common::ObSEArray<double, 16> building_relevances_;
  int64_t cached_cur_idx_;
  int64_t cached_token_doc_cnt_;
  double cached_max_token_relevance_;
  bool is_inited_;
  DISALLOW_COPY_AND_ASSIGN(ObTextRetrievalDaaTTokenIter);
};

class ObTextRetrievalBlockMaxIter final : public ObISRDimBlockMaxIter
{
public:
  ObTextRetrievalBlockMaxIter();
  ~ObTextRetrievalBlockMaxIter() {}
  int init(
      const ObTextRetrievalScanIterParam &iter_param,
      const ObBlockMaxScoreIterParam &block_max_iter_param,
      ObTableScanParam &scan_param);
  void reset();
  void reuse();

  virtual int get_next_row() override;
  virtual int get_next_batch(const int64_t capacity, int64_t &count) override;
  virtual int advance_to(const ObDatum &id_datum) override;

  virtual int get_curr_score(double &score) const override;
  virtual int get_curr_id(const ObDatum *&id_datum) const override;
  virtual int get_dim_max_score(double &score) override;
  virtual int advance_shallow(const ObDatum &id_datum, const bool inclusive) override;
  virtual int get_curr_block_max_info(const ObMaxScoreTuple *&max_score_tuple) override;
  virtual bool in_shallow_status() const override;
  // currently, for text retrieval, total_doc_cnt and token_doc_cnt is required before block max calculation
  int init_block_max_iter(const int64_t total_doc_cnt, const double avg_doc_token_cnt);
private:
  int calc_dim_max_score(
      const ObBlockMaxScoreIterParam &block_max_iter_param,
      const ObBlockMaxBM25RankingParam &ranking_param,
      ObTableScanParam &scan_param);
private:
  ObTextRetrievalDaaTTokenIter token_iter_;
  ObBlockMaxScoreIterator block_max_iter_;
  const ObBlockMaxScoreIterParam *block_max_iter_param_;
  ObTableScanParam *block_max_scan_param_;
  ObBlockMaxBM25RankingParam ranking_param_;
  const ObDatum *curr_id_;
  const ObMaxScoreTuple *max_score_tuple_;
  double dim_max_score_;
  bool block_max_inited_;
  bool in_shallow_status_;
  bool is_inited_;
  DISALLOW_COPY_AND_ASSIGN(ObTextRetrievalBlockMaxIter);
};

} // namespace storage
} // namespace oceanbase


#endif
