#pragma once

#include "lib/allocator/page_arena.h"
#include "lib/task/ob_timer.h"
#include "lib/string/ob_string.h"

namespace oceanbase
{
namespace sql
{

// Background half of the AI column pipeline.
//
// The DML side only writes a task row into oceanbase.__all_ai_column_task.
// This worker picks the due rows up, evaluates the argument expression of the
// AI function, calls the model, and publishes the result back into the column
// and its two state columns.
class ObAiColumnWorker : private common::ObTimerTask
{
public:
  ObAiColumnWorker();
  virtual ~ObAiColumnWorker() = default;

  int schedule(common::ObTimer &timer);

private:
  void runTimerTask() override;
  int run_once();
  int process_group(const uint64_t table_id,
                    const common::ObString &request_id,
                    const uint64_t column_id,
                    const common::ObString &column_name,
                    const common::ObString &rowkey_predicate,
                    const common::ObString &database_name,
                    const common::ObString &table_name,
                    const int64_t retry_count);
  int publish(const uint64_t table_id,
              const common::ObString &request_id,
              const uint64_t column_id,
              const common::ObString &column_name,
              const common::ObString &rowkey_predicate,
              const common::ObString &database_name,
              const common::ObString &table_name,
              const common::ObString &answer);
  int record_failure(const uint64_t table_id,
                     const common::ObString &request_id,
                     const uint64_t column_id,
                     const int64_t retry_count,
                     const int64_t max_retries,
                     const int error_code);
  int drop_task(const uint64_t table_id,
                const common::ObString &request_id,
                const uint64_t column_id);

private:
  // One second keeps the table tests responsive without busy polling.
  static constexpr int64_t SCHEDULE_PERIOD = 1000L * 1000L;
  static constexpr int64_t MAX_TASKS_PER_ROUND = 32;
  common::ObArenaAllocator alloc_;
};

} // namespace sql
} // namespace oceanbase
