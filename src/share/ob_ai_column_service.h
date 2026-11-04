/**
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

#pragma once

#include "lib/string/ob_string.h"
#include "lib/allocator/page_arena.h"
#include "lib/container/ob_array.h"
#include "lib/lock/ob_spin_lock.h"
#include "lib/task/ob_timer.h"

namespace oceanbase
{
namespace common
{
class ObMySQLProxy;
}
namespace share
{
namespace schema
{
class ObTableSchema;
class ObColumnSchemaV2;
}

// One pending AI column computation task of a table. When column_name_ is
// empty, all AI columns of the table are refreshed, otherwise only the given
// column. where_cond_ is an optional extra condition text which restricts the
// rows to recompute, used by ALTER TABLE ... RETRY AI COLUMN.
struct AIColumnTask
{
  AIColumnTask() : table_id_(0), column_name_(), where_cond_() {}
  uint64_t table_id_;
  common::ObString column_name_;
  common::ObString where_cond_;
};

// Async AI column background service.
//
// After a DML touches a table with AI columns (or a RETRY statement resets
// rows), a table level task is submitted here. The timer worker derives the
// work from data state: for every AI column it runs internal SQL
//   UPDATE <db>.<tbl> SET <col> = <ai expr>
//          WHERE <col> IS NULL [AND (<cond>)] LIMIT <batch>
// so the AI expression is evaluated by the SQL engine and the result goes
// back through the normal DML pipeline (vector indexes included). Rows whose
// AI column is still NULL are pending; a periodic full scan re-submits tasks
// so tasks are recovered after a server restart.
//
// If a task keeps failing for max_retries consecutive attempts (recorded in
// memory), it is marked FAILED and stops retrying until
// ALTER TABLE ... RETRY AI COLUMN resets it.
class AIColumnService : public common::ObTimerTask
{
public:
  static AIColumnService &get_instance();

  // schedule the timer task, sql_proxy is kept for internal SQL execution
  int start_if_needed(common::ObMySQLProxy &sql_proxy);
  void stop_and_wait();

  // submit a table level task, the strings are deep copied;
  // thread safe, can be called from SQL execution threads
  int submit_table_task(uint64_t table_id,
                        const common::ObString &column_name = common::ObString(),
                        const common::ObString &where_cond = common::ObString());
  // clear the FAILED state of the given AI column and resubmit its task
  int reset_retry_state(uint64_t table_id,
                        const common::ObString &column_name,
                        const common::ObString &where_cond = common::ObString());
  // notify that the given table was modified; cheap no-op when the table is
  // known to have no AI column
  int notify_dml(uint64_t table_id);

  virtual void runTimerTask() override;

private:
  AIColumnService();
  ~AIColumnService();

  struct RetryStat
  {
    RetryStat() : table_id_(0), column_name_(), fail_count_(0), failed_(false) {}
    uint64_t table_id_;
    common::ObString column_name_; // deep copied into task_allocator_
    int64_t fail_count_;
    bool failed_;
  };

  int do_full_scan();
  int process_tasks();
  int process_table_task(const AIColumnTask &task, bool &need_resubmit);
  int compute_ai_column(const schema::ObTableSchema &table_schema,
                        const schema::ObColumnSchemaV2 &column_schema,
                        const common::ObString &db_name,
                        const common::ObString &where_cond,
                        int64_t &max_retries,
                        bool &need_resubmit);
  int register_ai_table(uint64_t table_id);

  bool inited_;
  bool started_;
  bool first_scan_done_;
  int64_t tick_;
  common::ObMySQLProxy *sql_proxy_;
  common::ObTimer timer_;
  common::ObSpinLock lock_;                 // protects tasks_ / retry_stats_ / ai_table_ids_ / allocator
  common::ObArenaAllocator task_allocator_; // deep copy of task strings and retry column names
  common::ObArray<AIColumnTask> tasks_;
  common::ObArray<RetryStat> retry_stats_;
  common::ObArray<uint64_t> ai_table_ids_;
  common::ObArray<uint64_t> no_ai_table_ids_;
};

}  // namespace share
}  // namespace oceanbase
