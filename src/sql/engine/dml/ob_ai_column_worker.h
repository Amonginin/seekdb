#pragma once

#include "lib/allocator/page_arena.h"
#include "lib/container/ob_array.h"
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
// AI function, calls the model, and publishes the result back into the column.
// The task rows of one row share a request id, so the columns of a row are
// published together and a reader never sees half of them.
class ObAiColumnWorker : private common::ObTimerTask
{
public:
  ObAiColumnWorker();
  virtual ~ObAiColumnWorker() = default;

  int schedule(common::ObTimer &timer);

private:
  void runTimerTask() override;
  int run_once();

private:
  // A short period matters: the prompt of a request is evaluated when the
  // worker picks it up, so a request that waits too long would be answered
  // from a row that has already changed.
  static constexpr int64_t SCHEDULE_PERIOD = 100L * 1000L;
  static constexpr int64_t MAX_TASKS_PER_ROUND = 32;
  common::ObArenaAllocator alloc_;
};

} // namespace sql
} // namespace oceanbase
