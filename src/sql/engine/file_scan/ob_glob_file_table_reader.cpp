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

#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/file_scan/ob_glob_file_table_reader.h"
#include "share/ob_errno.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/stat.h>
#include <sys/types.h>

#include "lib/charset/ob_charset.h"
#include "lib/ob_define.h"
#include "lib/utility/ob_print_utils.h"
#include "lib/utility/ob_sort.h"

namespace oceanbase
{
using namespace common;
namespace sql
{

namespace
{
constexpr int64_t MAX_GLOB_PATH_LEN = 4096;

int dup_string(ObIAllocator &alloc, const ObString &src, ObString &dst)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(src.length() > INT32_MAX)) {
    ret = OB_SIZE_OVERFLOW;
  } else if (src.empty()) {
    dst = src;
  } else {
    char *buf = static_cast<char *>(alloc.alloc(src.length() + 1));
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("failed to alloc memory for string", K(src.length()), K(ret));
    } else {
      MEMCPY(buf, src.ptr(), src.length());
      buf[src.length()] = '\0';
      dst.assign_ptr(buf, static_cast<int32_t>(src.length()));
    }
  }
  return ret;
}

// lexicographic order of the matched file paths
struct PathLess
{
  bool operator()(const ObString &a, const ObString &b) const
  {
    return a.compare(b) < 0;
  }
};

// try to match one character: the pattern element at pat[p] against str[s];
// return the pattern element length on success, -1 on mismatch. Besides
// plain characters, '[...]' character classes are supported: '!' (or '^')
// negates the class, 'a-z' forms a range, and a ']' right after '[' or '[!'
// is a literal class member.
int64_t match_single(const char *pat, const int64_t pat_len, const int64_t p, const char *str,
    const int64_t s)
{
  int64_t ret_len = -1;
  if ('[' == pat[p]) {
    int64_t q = p + 1;
    bool neg = false;
    if (q < pat_len && ('!' == pat[q] || '^' == pat[q])) {
      neg = true;
      ++q;
    }
    bool matched = false;
    bool first = true;
    while (q < pat_len && (first || ']' != pat[q])) {
      first = false;
      if (q + 2 < pat_len && ']' != pat[q + 1] && '-' == pat[q + 1] && ']' != pat[q + 2]) {
        // a character range like a-z
        if (str[s] >= pat[q] && str[s] <= pat[q + 2]) {
          matched = true;
        }
        q += 3;
      } else {
        if (str[s] == pat[q]) {
          matched = true;
        }
        ++q;
      }
    }
    if (q < pat_len) {
      // the closing ']' was found
      ++q;
      if (matched != neg) {
        ret_len = q - p;
      }
    }
  } else if (pat[p] == str[s]) {
    ret_len = 1;
  }
  return ret_len;
}

// wildcard match with '*' / '?' / '[...]' support (iterative, with star
// backtracking), equivalent to fnmatch() without special flags: '*' matches
// any sequence of characters (including none), '?' matches exactly one
// character
bool glob_match(const char *pat, const int64_t pat_len, const char *str, const int64_t str_len)
{
  int64_t p = 0;
  int64_t s = 0;
  int64_t star_p = -1;
  int64_t star_s = 0;
  while (s < str_len) {
    bool advanced = false;
    if (p < pat_len && '*' == pat[p]) {
      // remember the star position and first try to match zero characters
      star_p = p;
      ++p;
      star_s = s;
      advanced = true;
    }
    if (!advanced && p < pat_len) {
      if ('?' == pat[p]) {
        ++p;
        ++s;
        advanced = true;
      } else {
        const int64_t elem_len = match_single(pat, pat_len, p, str, s);
        if (elem_len > 0) {
          p += elem_len;
          ++s;
          advanced = true;
        }
      }
    }
    if (!advanced) {
      if (star_p < 0) {
        return false;
      }
      // backtrack: let the last '*' consume one more character
      p = star_p + 1;
      s = ++star_s;
    }
  }
  // only trailing stars may remain in the pattern
  while (p < pat_len && '*' == pat[p]) {
    ++p;
  }
  return p == pat_len;
}
} // end anonymous namespace

GlobFileTableReader::GlobFileTableReader(ObIAllocator &alloc)
    : alloc_(alloc),
      next_idx_(0),
      rows_emitted_(0),
      inited_(false)
{}

GlobFileTableReader::~GlobFileTableReader()
{
  close();
}

int GlobFileTableReader::init(const ObString &path, const FileReadOptions &options)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(path.empty())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("glob pattern is empty", K(ret));
  } else if (OB_FAIL(dup_string(alloc_, path, path_))) {
    LOG_WARN("failed to copy glob pattern", K(ret));
  } else {
    options_.max_rows_ = options.max_rows_;
    if (OB_FAIL(dup_string(alloc_, options.secure_root_, options_.secure_root_))) {
      return ret;
    }
    inited_ = true;
  }
  if (OB_SUCC(ret)) {
    // fixed schema: one "file" column containing matched file paths
    if (0 == columns_.count()) {
      FileColumnMeta col;
      col.name_ = ObString("file");
      col.type_.set_varchar();
      col.type_.set_collation_type(CS_TYPE_UTF8MB4_BIN);
      if (OB_FAIL(columns_.push_back(col))) {
        LOG_WARN("failed to push file column meta", K(ret));
      }
    }
  }
  if (OB_SUCC(ret)) {
    if (OB_FAIL(scan_files_())) {
      LOG_WARN("failed to scan glob files", K(path_), K(ret));
    }
  }
  if (OB_FAIL(ret)) {
    close();
  }
  return ret;
}

int GlobFileTableReader::get_column_metas(ObIArray<FileColumnMeta> &columns) const
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("glob file table reader is not inited", K(ret));
  } else if (OB_FAIL(columns.assign(columns_))) {
    LOG_WARN("failed to assign column metas", K(ret));
  }
  return ret;
}

int GlobFileTableReader::get_next_row(ObIArray<ObObj> &row, bool &has_next)
{
  int ret = OB_SUCCESS;
  has_next = false;
  row.reuse();
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("glob file table reader is not inited", K(ret));
  } else if (options_.max_rows_ > 0 && rows_emitted_ >= options_.max_rows_) {
    // max rows reached
  } else if (next_idx_ >= matched_files_.count()) {
    // iteration end
  } else {
    const ObString &file_path = matched_files_.at(next_idx_);
    ObObj obj;
    obj.set_varchar(file_path);
    obj.meta_.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (OB_FAIL(row.push_back(obj))) {
      LOG_WARN("failed to push glob row cell", K(ret));
    } else {
      ++next_idx_;
      ++rows_emitted_;
      has_next = true;
    }
  }
  return ret;
}

int GlobFileTableReader::scan_files_()
{
  int ret = OB_SUCCESS;
  matched_files_.reuse();
  next_idx_ = 0;
  rows_emitted_ = 0;
  // split the pattern into a literal directory prefix and the last path
  // component pattern: the first wildcard character marks the component part,
  // and the last slash before it ends the directory prefix
  int64_t glob_pos = path_.length();
  for (int64_t i = 0; i < path_.length(); ++i) {
    if ('*' == path_.ptr()[i] || '?' == path_.ptr()[i] || '[' == path_.ptr()[i]) {
      glob_pos = i;
      break;
    }
  }
  int64_t slash_pos = -1;
  for (int64_t i = glob_pos - 1; i >= 0; --i) {
    if ('/' == path_.ptr()[i]) {
      slash_pos = i;
      break;
    }
  }
  ObString dir;
  ObString pattern;
  if (slash_pos < 0) {
    dir = ObString(".");
    pattern = path_;
  } else {
    dir.assign_ptr(path_.ptr(), static_cast<int32_t>(slash_pos));
    pattern.assign_ptr(path_.ptr() + slash_pos + 1,
        static_cast<int32_t>(path_.length() - slash_pos - 1));
  }
  if (OB_SUCC(ret)) {
    // wildcards inside a middle path component are not supported
    for (int64_t i = 0; OB_SUCC(ret) && i < pattern.length(); ++i) {
      if ('/' == pattern.ptr()[i]) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED,
            "wildcards in middle path components are not supported");
        LOG_WARN("glob wildcard in a middle path component", K(pattern), K(ret));
      }
    }
  }
  // the directory prefix must be a NUL terminated path for opendir()
  char dir_buf[MAX_GLOB_PATH_LEN];
  if (OB_SUCC(ret)) {
    if (OB_UNLIKELY(dir.length() + 1 > MAX_GLOB_PATH_LEN)) {
      ret = OB_SIZE_OVERFLOW;
      LOG_WARN("glob directory prefix is too long", K(dir.length()), K(ret));
    } else {
      MEMCPY(dir_buf, dir.ptr(), static_cast<size_t>(dir.length()));
      dir_buf[dir.length()] = '\0';
    }
  }
  if (OB_SUCC(ret)) {
    ::DIR *dirp = ::opendir(dir_buf);
    if (OB_ISNULL(dirp)) {
      ret = OB_FILE_NOT_EXIST;
      LOG_USER_ERROR(OB_FILE_NOT_EXIST, static_cast<int>(dir.length()), dir.ptr());
      LOG_WARN("failed to open glob directory", K(dir), K(ret));
    } else {
      struct dirent *entry = NULL;
      while (OB_SUCC(ret) && NULL != (entry = ::readdir(dirp))) {
        const int64_t name_len = static_cast<int64_t>(strlen(entry->d_name));
        if (0 == strcmp(entry->d_name, ".") || 0 == strcmp(entry->d_name, "..")) {
          continue;
        } else if (OB_UNLIKELY(dir.length() + 1 + name_len + 1 > MAX_GLOB_PATH_LEN)) {
          // skip paths that are too long
          LOG_WARN("glob matched path is too long, skipped", K(dir), K(name_len));
          continue;
        } else if (!glob_match(pattern.ptr(), pattern.length(), entry->d_name, name_len)) {
          continue;
        }
        // build "<dir>/<name>" and keep regular files only
        char full_path[MAX_GLOB_PATH_LEN];
        MEMCPY(full_path, dir.ptr(), static_cast<size_t>(dir.length()));
        full_path[dir.length()] = '/';
        MEMCPY(full_path + dir.length() + 1, entry->d_name, static_cast<size_t>(name_len));
        full_path[dir.length() + 1 + name_len] = '\0';
        struct stat st;
        if (0 != ::stat(full_path, &st) || !S_ISREG(st.st_mode)) {
          continue;
        }
        char canonical[MAX_GLOB_PATH_LEN];
        if (nullptr == ::realpath(full_path, canonical)) {
          ret = OB_FILE_NOT_EXIST;
          break;
        }
        const ObString resolved(canonical);
        const ObString &root = options_.secure_root_;
        if (!root.empty() && (!resolved.prefix_match(root)
            || (root != "/" && resolved.length() > root.length() && resolved[root.length()] != '/'))) {
          ret = OB_ERR_NO_PRIVILEGE;
          break;
        }
        if (matched_files_.count() >= OB_FILE_TABLE_MAX_FILE_COUNT) {
          ret = OB_SIZE_OVERFLOW;
          LOG_USER_ERROR(OB_SIZE_OVERFLOW);
          LOG_WARN("too many files matched by the glob pattern", K(path_),
              K(OB_FILE_TABLE_MAX_FILE_COUNT), K(ret));
          break;
        }
        char *path_buf =
            static_cast<char *>(alloc_.alloc(dir.length() + 1 + name_len));
        if (OB_ISNULL(path_buf)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
          LOG_WARN("failed to alloc glob matched path", K(ret));
        } else {
          MEMCPY(path_buf, full_path, static_cast<size_t>(dir.length() + 1 + name_len));
          ObString matched(static_cast<int32_t>(dir.length() + 1 + name_len), path_buf);
          if (OB_FAIL(matched_files_.push_back(matched))) {
            LOG_WARN("failed to push matched file path", K(ret));
          }
        }
      }
      ::closedir(dirp);
    }
  }
  if (OB_SUCC(ret) && matched_files_.count() > 1) {
    // sort the matched paths so that the iteration order is deterministic
    lib::ob_sort(&matched_files_.at(0), &matched_files_.at(0) + matched_files_.count(), PathLess());
  }
  return ret;
}

int GlobFileTableReader::reuse()
{
  // rewind to the first matched file, the collected files are kept
  next_idx_ = 0;
  rows_emitted_ = 0;
  return OB_SUCCESS;
}

int GlobFileTableReader::get_file_stat(FileTableFileStat &stat) const
{
  // a glob pattern has no meaningful single file stat
  stat = FileTableFileStat();
  return OB_SUCCESS;
}

void GlobFileTableReader::close()
{
  inited_ = false;
  next_idx_ = 0;
  rows_emitted_ = 0;
}

} // end namespace sql
} // end namespace oceanbase
