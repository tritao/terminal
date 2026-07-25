#include "terminal_runtime.h"
#include "../terminal_core.h"

#include <stdlib.h>

struct terminal_runtime {
  terminal_core_t* core;
};

terminal_runtime_t* terminal_runtime_new(
    int columns, int rows, int scrollback_limit, const char* term,
    const char* command, const char** arguments, const char** environment,
    const char* cwd) {
  terminal_runtime_t* runtime = calloc(1, sizeof(*runtime));
  if (!runtime)
    return NULL;
  runtime->core = terminal_core_new(columns, rows, scrollback_limit, term,
    command, arguments, environment, cwd);
  if (!runtime->core) {
    free(runtime);
    return NULL;
  }
  return runtime;
}

void terminal_runtime_free(terminal_runtime_t* runtime) {
  if (!runtime)
    return;
  terminal_core_free(runtime->core);
  free(runtime);
}

int terminal_runtime_write(terminal_runtime_t* runtime,
    const char* data, size_t length) {
  return runtime ? terminal_core_feed(runtime->core, data, (int)length) : 0;
}

void terminal_runtime_resize(terminal_runtime_t* runtime,
    int columns, int rows) {
  if (runtime)
    terminal_core_resize(runtime->core, columns, rows);
}

int terminal_runtime_poll(terminal_runtime_t* runtime,
    terminal_runtime_output_callback callback, void* user_data,
    int* total_shifts) {
  if (!runtime)
    return 0;
  return terminal_core_update(runtime->core, callback, user_data, total_shifts);
}

int terminal_runtime_exited(terminal_runtime_t* runtime,
    int* exit_code, int* signal) {
  return runtime ? terminal_core_exited(runtime->core, exit_code, signal) : 0;
}

int terminal_runtime_close(terminal_runtime_t* runtime) {
  return runtime ? terminal_core_close(runtime->core) : 0;
}
