#pragma once

#include "lib/container/ob_se_array.h"
#include "lib/string/ob_string.h"

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

// DML side of the AI column pipeline. A row that is inserted (or whose
// dependency columns change) gets one task row per AI column in
// oceanbase.__all_ai_column_task. The task row is written through a nested
// session, so it belongs to the transaction of the statement itself: it is
// visible before COMMIT and disappears on ROLLBACK.
class ObAiColumnRuntime
{
public:
  // Creates the task rows for one inserted row. `row` is the evaluated row of
  // the insert, in the layout of `ctdef.column_ids_`.
  static int create_tasks_for_insert(ObTableModifyOp &op,
                                     const ObDMLBaseCtDef &ctdef,
                                     const common::ObIArray<ObExpr *> &row,
                                     common::ObIAllocator &alloc);
};

} // namespace sql
} // namespace oceanbase
