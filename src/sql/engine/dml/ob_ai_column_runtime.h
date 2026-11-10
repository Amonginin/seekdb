#pragma once

#include "lib/container/ob_se_array.h"
#include "lib/string/ob_string.h"
#include "share/ob_define.h"

namespace oceanbase
{
namespace common
{
class ObIAllocator;
}
namespace sql
{

class ObTableModifyOp;
struct ObDMLBaseCtDef;

// Diagnostic sink of the AI column path. seekdb drops the usual warning log in
// the deployment used for testing, so the messages go to a plain file.
void ob_ai_trace(const char *fmt, ...);

// One row that needs its AI columns recomputed. Only the identifying
// information is kept: table, kind of change, the rowkey predicate and the
// columns that really changed. Nothing here executes SQL.
class ObAiPendingRow
{
public:
  ObAiPendingRow()
    : table_id_(common::OB_INVALID_ID),
      dml_event_(0)
  {}
  TO_STRING_KV(K_(table_id), K_(dml_event), K_(rowkey_predicate),
               K_(old_rowkey_predicate), K_(changed_column_ids), K_(cleared_column_ids));
  uint64_t table_id_;
  int64_t dml_event_;
  common::ObString rowkey_predicate_;
  // Only set for an update: the predicate before the change, so a task of the
  // old row can be dropped when the primary key moved.
  common::ObString old_rowkey_predicate_;
  common::ObString changed_column_ids_;
  // The columns whose new value is null. Clearing an AI column asks for it to
  // be computed again; writing a result into it does not.
  common::ObString cleared_column_ids_;
  // Names of the touched table, used to mark the row with its request id.
  common::ObString database_name_;
  common::ObString table_name_;
};
typedef common::ObSEArray<ObAiPendingRow, 8> ObAiPendingRowArray;

// DML side of the AI column pipeline, in two phases.
//
// Phase one runs while the rows are produced and only records the row keys,
// which keeps the DML statement free of any nested execution.
//
// Phase two runs once the statement has finished writing its own rows and
// still shares its transaction: the task rows of
// oceanbase.__all_ai_column_task are written there. Because the write uses the
// connection of the running session, the task rows are visible before COMMIT
// and disappear on ROLLBACK.
class ObAiColumnRuntime
{
public:
  static int record_row(ObTableModifyOp &op,
                        const ObDMLBaseCtDef &ctdef,
                        const int64_t dml_event,
                        common::ObIAllocator &alloc,
                        ObAiPendingRowArray &pending);

  static int flush_pending(ObTableModifyOp &op,
                           const ObAiPendingRowArray &pending,
                           common::ObIAllocator &alloc,
                           bool &has_work);
};

} // namespace sql
} // namespace oceanbase
