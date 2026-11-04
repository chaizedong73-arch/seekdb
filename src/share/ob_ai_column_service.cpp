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

#include "share/ob_ai_column_service.h"

#include "lib/string/ob_sql_string.h"
#include "share/ob_server_struct.h"
#include "share/schema/ob_ai_column_meta.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_schema_struct.h"

namespace oceanbase
{
using namespace common;
using namespace share::schema;

namespace share
{

static const int64_t AI_COLUMN_TICK_INTERVAL_US = 1000 * 1000L; // 1s
static const int64_t AI_COLUMN_FULL_SCAN_TICKS = 60;            // full scan every 60s
static const int64_t AI_COLUMN_BATCH_ROW_LIMIT = 100;

static int ai_deep_copy_string(ObIAllocator &allocator,
                               const ObString &src,
                               ObString &dst)
{
  int ret = OB_SUCCESS;
  if (!src.empty()) {
    char *buf = static_cast<char *>(allocator.alloc(src.length()));
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("fail to alloc memory for ai column task string", K(ret), K(src.length()));
    } else {
      MEMCPY(buf, src.ptr(), src.length());
      dst.assign_ptr(buf, static_cast<int32_t>(src.length()));
    }
  }
  return ret;
}

AIColumnService &AIColumnService::get_instance()
{
  static AIColumnService instance;
  return instance;
}

AIColumnService::AIColumnService()
    : inited_(false), started_(false), first_scan_done_(false), tick_(0),
      sql_proxy_(NULL), timer_(), lock_(), task_allocator_("AIColumnTask"),
      tasks_(), retry_stats_(), ai_table_ids_(), no_ai_table_ids_()
{}

AIColumnService::~AIColumnService()
{
  stop_and_wait();
}

int AIColumnService::start_if_needed(ObMySQLProxy &sql_proxy)
{
  int ret = OB_SUCCESS;
  if (started_) {
    // already started
  } else {
    ObSpinLockGuard guard(lock_);
    if (started_) {
      // started by another thread
    } else if (OB_FAIL(timer_.init("AIColSvc", ObMemAttr("AIColSvc")))) {
      LOG_WARN("fail to init ai column timer", K(ret));
    } else {
      sql_proxy_ = &sql_proxy;
      disable_timeout_check();
      if (OB_FAIL(timer_.schedule(*this, AI_COLUMN_TICK_INTERVAL_US,
                                  true /*repeat*/, true /*immediate*/))) {
        LOG_WARN("fail to schedule ai column timer task", K(ret));
        timer_.destroy();
        sql_proxy_ = NULL;
      } else {
        inited_ = true;
        started_ = true;
      }
    }
  }
  return ret;
}

void AIColumnService::stop_and_wait()
{
  if (started_) {
    timer_.stop();
    timer_.wait();
    timer_.destroy();
    started_ = false;
  }
}

int AIColumnService::submit_table_task(uint64_t table_id,
                                       const ObString &column_name,
                                       const ObString &where_cond)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(sql_proxy_)) {
    // lazy start for the case that the observer module init did not run yet
    if (OB_ISNULL(GCTX.sql_proxy_)) {
      LOG_WARN("ai column service is not started and no sql proxy", K(ret), K(table_id));
    } else if (OB_FAIL(start_if_needed(*GCTX.sql_proxy_))) {
      LOG_WARN("fail to start ai column service", K(ret));
    }
  }
  if (OB_SUCC(ret)) {
    ObSpinLockGuard guard(lock_);
    bool duplicate = false;
    for (int64_t i = 0; !duplicate && i < tasks_.count(); ++i) {
      if (tasks_.at(i).table_id_ == table_id
          && 0 == tasks_.at(i).column_name_.case_compare(column_name)
          && 0 == tasks_.at(i).where_cond_.case_compare(where_cond)) {
        duplicate = true;
      }
    }
    if (!duplicate) {
      AIColumnTask task;
      task.table_id_ = table_id;
      if (OB_FAIL(ai_deep_copy_string(task_allocator_, column_name, task.column_name_))) {
        LOG_WARN("fail to deep copy ai column name", K(ret), K(column_name));
      } else if (OB_FAIL(ai_deep_copy_string(task_allocator_, where_cond, task.where_cond_))) {
        LOG_WARN("fail to deep copy ai column where cond", K(ret), K(where_cond));
      } else if (OB_FAIL(tasks_.push_back(task))) {
        LOG_WARN("fail to push ai column task", K(ret), K(table_id));
      }
    }
  }
  return ret;
}

int AIColumnService::reset_retry_state(uint64_t table_id,
                                       const ObString &column_name,
                                       const ObString &where_cond)
{
  int ret = OB_SUCCESS;
  {
    ObSpinLockGuard guard(lock_);
    for (int64_t i = 0; i < retry_stats_.count(); ++i) {
      if (retry_stats_.at(i).table_id_ == table_id
          && 0 == retry_stats_.at(i).column_name_.case_compare(column_name)) {
        retry_stats_.at(i).fail_count_ = 0;
        retry_stats_.at(i).failed_ = false;
      }
    }
  }
  if (OB_SUCC(ret)) {
    if (OB_FAIL(submit_table_task(table_id, column_name, where_cond))) {
      LOG_WARN("fail to resubmit ai column task after retry reset", K(ret), K(table_id), K(column_name));
    }
  }
  return ret;
}

bool AIColumnService::has_ai_column_table(uint64_t table_id)
{
  ObSpinLockGuard guard(lock_);
  for (int64_t i = 0; i < ai_table_ids_.count(); ++i) {
    if (ai_table_ids_.at(i) == table_id) {
      return true;
    }
  }
  return false;
}

int AIColumnService::notify_dml(uint64_t table_id)
{
  int ret = OB_SUCCESS;
  if (has_ai_column_table(table_id)) {
    if (OB_FAIL(submit_table_task(table_id))) {
      LOG_WARN("fail to submit ai column task after dml", K(ret), K(table_id));
    }
  } else {
    // unknown table, check its schema once, the result is negatively cached
    // until the next full scan refreshes it
    bool has_ai_col = false;
    bool checked = false;
    {
      ObSpinLockGuard guard(lock_);
      for (int64_t i = 0; !checked && i < no_ai_table_ids_.count(); ++i) {
        if (no_ai_table_ids_.at(i) == table_id) {
          checked = true;
        }
      }
    }
    if (!checked && OB_NOT_NULL(GCTX.schema_service_)) {
      ObSchemaGetterGuard schema_guard;
      const ObTableSchema *table_schema = NULL;
      if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(schema_guard))) {
        LOG_WARN("fail to get schema guard for ai column notify", K(ret), K(table_id));
      } else if (OB_FAIL(schema_guard.get_table_schema(table_id, table_schema))) {
        LOG_WARN("fail to get table schema for ai column notify", K(ret), K(table_id));
      } else if (OB_ISNULL(table_schema)) {
        // table does not exist, nothing to do
      } else {
        for (ObTableSchema::const_column_iterator it = table_schema->column_begin();
             !has_ai_col && OB_SUCC(ret) && it != table_schema->column_end(); ++it) {
          if (OB_NOT_NULL(*it) && (*it)->is_ai_column()) {
            has_ai_col = true;
          }
        }
        if (OB_SUCC(ret)) {
          ObSpinLockGuard guard(lock_);
          if (has_ai_col) {
            if (OB_FAIL(ai_table_ids_.push_back(table_id))) {
              LOG_WARN("fail to register ai column table", K(ret), K(table_id));
            }
          } else if (OB_FAIL(no_ai_table_ids_.push_back(table_id))) {
            LOG_WARN("fail to record non ai column table", K(ret), K(table_id));
          }
        }
      }
      if (OB_SUCC(ret) && has_ai_col) {
        if (OB_FAIL(submit_table_task(table_id))) {
          LOG_WARN("fail to submit ai column task after dml", K(ret), K(table_id));
        }
      }
    }
  }
  return ret;
}

void AIColumnService::runTimerTask()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!inited_) || OB_ISNULL(sql_proxy_)) {
    // not started yet
  } else {
    tick_++;
    if (OB_FAIL(do_full_scan())) {
      LOG_WARN("fail to do ai column full scan", K(ret));
    }
    if (OB_FAIL(process_tasks())) {
      LOG_WARN("fail to process ai column tasks", K(ret));
    }
  }
}

int AIColumnService::do_full_scan()
{
  int ret = OB_SUCCESS;
  if (first_scan_done_ && (tick_ % AI_COLUMN_FULL_SCAN_TICKS) != 0) {
    // full scan is not due
  } else if (OB_ISNULL(GCTX.schema_service_)) {
    // server is not ready yet, retry on the next tick
  } else {
    ObSchemaGetterGuard schema_guard;
    ObArray<const ObTableSchema *> tables;
    if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(schema_guard))) {
      LOG_WARN("fail to get schema guard for ai column full scan", K(ret));
    } else if (OB_FAIL(schema_guard.get_table_schemas_in_runtime(tables))) {
      LOG_WARN("fail to get table schemas for ai column full scan", K(ret));
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < tables.count(); ++i) {
        const ObTableSchema *table = tables.at(i);
        bool has_ai_col = false;
        if (OB_ISNULL(table) || !table->is_user_table()) {
          // skip inner tables and views
        } else {
          for (ObTableSchema::const_column_iterator it = table->column_begin();
               !has_ai_col && OB_SUCC(ret) && it != table->column_end(); ++it) {
            if (OB_NOT_NULL(*it) && (*it)->is_ai_column()) {
              has_ai_col = true;
            }
          }
        }
        if (OB_SUCC(ret) && has_ai_col) {
          {
            ObSpinLockGuard guard(lock_);
            // negative cache is refreshed by this scan
            no_ai_table_ids_.reset();
          }
          if (OB_FAIL(register_ai_table(table->get_table_id()))) {
            LOG_WARN("fail to register ai column table", K(ret), K(table->get_table_id()));
          } else if (OB_FAIL(submit_table_task(table->get_table_id()))) {
            LOG_WARN("fail to submit ai column table task", K(ret), K(table->get_table_id()));
          }
        }
      }
      if (OB_SUCC(ret)) {
        first_scan_done_ = true;
      }
    }
  }
  return ret;
}

int AIColumnService::register_ai_table(uint64_t table_id)
{
  int ret = OB_SUCCESS;
  ObSpinLockGuard guard(lock_);
  bool exists = false;
  for (int64_t i = 0; !exists && i < ai_table_ids_.count(); ++i) {
    if (ai_table_ids_.at(i) == table_id) {
      exists = true;
    }
  }
  if (!exists && OB_FAIL(ai_table_ids_.push_back(table_id))) {
    LOG_WARN("fail to register ai column table", K(ret), K(table_id));
  }
  return ret;
}

int AIColumnService::process_tasks()
{
  int ret = OB_SUCCESS;
  ObArray<AIColumnTask> batch;
  {
    ObSpinLockGuard guard(lock_);
    if (OB_FAIL(batch.assign(tasks_))) {
      LOG_WARN("fail to copy ai column tasks", K(ret));
    } else {
      tasks_.reset();
    }
  }
  if (OB_SUCC(ret)) {
    for (int64_t i = 0; OB_SUCC(ret) && i < batch.count(); ++i) {
      bool need_resubmit = false;
      if (OB_FAIL(process_table_task(batch.at(i), need_resubmit))) {
        LOG_WARN("fail to process ai column table task", K(ret), K(batch.at(i).table_id_));
      } else if (need_resubmit) {
        if (OB_FAIL(submit_table_task(batch.at(i).table_id_,
                                      batch.at(i).column_name_,
                                      batch.at(i).where_cond_))) {
          LOG_WARN("fail to resubmit ai column table task", K(ret), K(batch.at(i).table_id_));
        }
      }
    }
  }
  return ret;
}

int AIColumnService::process_table_task(const AIColumnTask &task, bool &need_resubmit)
{
  int ret = OB_SUCCESS;
  need_resubmit = false;
  ObSchemaGetterGuard schema_guard;
  const ObTableSchema *table_schema = NULL;
  const ObDatabaseSchema *db_schema = NULL;
  if (OB_ISNULL(GCTX.schema_service_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("schema service is null", K(ret));
  } else if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(schema_guard))) {
    LOG_WARN("fail to get schema guard", K(ret), K(task.table_id_));
  } else if (OB_FAIL(schema_guard.get_table_schema(task.table_id_, table_schema))) {
    LOG_WARN("fail to get table schema", K(ret), K(task.table_id_));
  } else if (OB_ISNULL(table_schema)) {
    // table is dropped, the task is done
  } else if (OB_FAIL(schema_guard.get_database_schema(table_schema->get_database_id(), db_schema))) {
    LOG_WARN("fail to get database schema", K(ret), K(task.table_id_));
  } else if (OB_ISNULL(db_schema)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("database schema is null", K(ret), K(task.table_id_));
  } else {
    for (ObTableSchema::const_column_iterator it = table_schema->column_begin();
         OB_SUCC(ret) && it != table_schema->column_end(); ++it) {
      const ObColumnSchemaV2 *column = *it;
      if (OB_ISNULL(column) || !column->is_ai_column()) {
        // not an ai column
      } else if (!task.column_name_.empty()
                 && 0 != column->get_column_name_str().case_compare(task.column_name_)) {
        // another ai column which is not requested by this task
      } else {
        bool failed = false;
        int64_t max_retries = AI_COLUMN_DEFAULT_MAX_RETRIES;
        bool col_need_resubmit = false;
        {
          ObSpinLockGuard guard(lock_);
          for (int64_t i = 0; i < retry_stats_.count(); ++i) {
            if (retry_stats_.at(i).table_id_ == task.table_id_
                && 0 == retry_stats_.at(i).column_name_.case_compare(column->get_column_name_str())) {
              failed = retry_stats_.at(i).failed_;
              break;
            }
          }
        }
        if (failed) {
          // the column is marked FAILED, wait for RETRY AI COLUMN
          LOG_WARN("ai column task is in FAILED state, skip it",
                   K(task.table_id_), K(column->get_column_name_str()));
        } else if (OB_FAIL(compute_ai_column(*table_schema, *column,
                                             db_schema->get_database_name_str(),
                                             task.where_cond_, max_retries,
                                             col_need_resubmit))) {
          // record one failure of this column, mark FAILED after max_retries
          bool mark_failed = false;
          {
            ObSpinLockGuard guard(lock_);
            RetryStat *stat = NULL;
            for (int64_t i = 0; OB_ISNULL(stat) && i < retry_stats_.count(); ++i) {
              if (retry_stats_.at(i).table_id_ == task.table_id_
                  && 0 == retry_stats_.at(i).column_name_.case_compare(column->get_column_name_str())) {
                stat = &retry_stats_.at(i);
              }
            }
            if (OB_ISNULL(stat)) {
              RetryStat new_stat;
              new_stat.table_id_ = task.table_id_;
              if (OB_SUCCESS != ai_deep_copy_string(task_allocator_,
                                                    column->get_column_name_str(),
                                                    new_stat.column_name_)) {
                LOG_WARN("fail to deep copy retry stat column name", K(task.table_id_));
              } else if (OB_FAIL(retry_stats_.push_back(new_stat))) {
                LOG_WARN("fail to push retry stat", K(task.table_id_));
              } else {
                stat = &retry_stats_.at(retry_stats_.count() - 1);
              }
            }
            if (OB_NOT_NULL(stat)) {
              stat->fail_count_++;
              if (stat->fail_count_ >= max_retries) {
                stat->failed_ = true;
                mark_failed = true;
              }
            }
          }
          if (mark_failed) {
            LOG_WARN("ai column task reaches max retries, mark it as FAILED",
                     K(task.table_id_), K(column->get_column_name_str()), K(max_retries));
          } else {
            need_resubmit = true;
          }
        } else if (col_need_resubmit) {
          need_resubmit = true;
        }
      }
    }
  }
  return ret;
}

int AIColumnService::compute_ai_column(const ObTableSchema &table_schema,
                                       const ObColumnSchemaV2 &column_schema,
                                       const ObString &db_name,
                                       const ObString &where_cond,
                                       int64_t &max_retries,
                                       bool &need_resubmit)
{
  int ret = OB_SUCCESS;
  need_resubmit = false;
  ObArenaAllocator meta_allocator("AIColMeta");
  ObString meta_json = column_schema.get_cur_default_value().get_string();
  AIColumnMeta ai_meta;
  const ObString col_name = column_schema.get_column_name_str();
  const ObString tbl_name = table_schema.get_table_name_str();
  if (meta_json.empty()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ai column has no meta json", K(ret), K(col_name));
  } else if (OB_FAIL(parse_ai_column_meta(meta_allocator, meta_json, ai_meta))) {
    LOG_WARN("fail to parse ai column meta", K(ret), K(col_name));
  } else {
    max_retries = ai_meta.max_retries_;
    ObSqlString sql;
    if (OB_FAIL(sql.append_fmt("UPDATE `%.*s`.`%.*s` SET `%.*s` = %.*s WHERE `%.*s` IS NULL",
                               db_name.length(), db_name.ptr(),
                               tbl_name.length(), tbl_name.ptr(),
                               col_name.length(), col_name.ptr(),
                               ai_meta.expr_.length(), ai_meta.expr_.ptr(),
                               col_name.length(), col_name.ptr()))) {
      LOG_WARN("fail to build ai column update sql", K(ret), K(col_name));
    } else if (!where_cond.empty()
               && OB_FAIL(sql.append_fmt(" AND (%.*s)", where_cond.length(), where_cond.ptr()))) {
      LOG_WARN("fail to append ai column where cond", K(ret), K(where_cond));
    } else if (OB_FAIL(sql.append_fmt(" LIMIT %ld", AI_COLUMN_BATCH_ROW_LIMIT))) {
      LOG_WARN("fail to append ai column update limit", K(ret));
    } else {
      int64_t affected_rows = 0;
      if (OB_FAIL(sql_proxy_->write(sql.string(), affected_rows))) {
        LOG_WARN("fail to run ai column update", K(ret), K(sql));
      } else if (affected_rows > 0) {
        // some rows may still have NULL values, process them on the next tick
        need_resubmit = true;
      }
    }
  }
  return ret;
}

}  // namespace share
}  // namespace oceanbase
