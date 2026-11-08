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

#ifndef _OB_AI_DIAG_H
#define _OB_AI_DIAG_H 1

#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

namespace oceanbase
{

// Temporary tracing helper for the AI column DDL path. The OceanBase log
// module swallows warnings in this build and stderr is redirected, so the trace
// goes straight to a file through the lowest level syscalls available.
class ObAiDiag
{
public:
  static void trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)))
  {
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (len < 0) {
      return;
    }
    if (len > static_cast<int>(sizeof(buf) - 1)) {
      len = static_cast<int>(sizeof(buf) - 1);
    }
    int fd = ::open("/tmp/aidbg.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
      ssize_t written = ::write(fd, buf, len);
      (void)written;
      ::close(fd);
    }
  }
};

} // namespace oceanbase

#endif // _OB_AI_DIAG_H
