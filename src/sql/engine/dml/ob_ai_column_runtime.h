#pragma once

#include "lib/container/ob_se_array.h"
#include "lib/string/ob_string.h"
#include "query/api/query/engine/expr/ob_expr.h"

namespace oceanbase
{
namespace common
{
class ObIAllocator;
}
namespace sql
{

class ObExpr;
class ObTableModifyOp;
struct ObDMLBaseCtDef;

// DML side of the AI column pipeline.
//
// The work runs from ObDMLService::handle_after_processing_single_row, not
// from the insert loop: by then the DAS buffer of the statement has been
// written, and that is the same point the foreign key checks use a nested
// session from. Doing it earlier corrupted the row being written.
class ObAiColumnRuntime
{
public:
  // A row that is inserted, or whose dependency columns changed, gets one task
  // row per AI column in oceanbase.__all_ai_column_task. The row is written in
  // a nested session, so it belongs to the transaction of the statement: it is
  // visible before COMMIT and disappears on ROLLBACK.
  static int schedule_row(ObTableModifyOp &op,
                          const ObDMLBaseCtDef &ctdef,
                          const int64_t dml_event);

  // True when the table has the hidden columns that back an AI column. The
  // check reads the schema, not the metadata table, so it stays cheap enough
  // to run for every modified row.
  static int has_ai_column(ObTableModifyOp &op,
                           const uint64_t table_id,
                           bool &has_ai_column);
};

} // namespace sql
} // namespace oceanbase
