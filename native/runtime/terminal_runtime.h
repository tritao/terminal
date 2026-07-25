#ifndef PRAGTICAL_TERMINAL_RUNTIME_H
#define PRAGTICAL_TERMINAL_RUNTIME_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct terminal_runtime terminal_runtime_t;
typedef void (*terminal_runtime_output_callback)(char* data, int length, void* user_data);
const char* terminal_runtime_last_error(void);

terminal_runtime_t* terminal_runtime_new(
  int columns,
  int rows,
  int scrollback_limit,
  const char* term,
  const char* command,
  const char** arguments,
  const char** environment,
  const char* cwd
);
void terminal_runtime_free(terminal_runtime_t* runtime);
int terminal_runtime_write(terminal_runtime_t* runtime, const char* data, size_t length);
void terminal_runtime_resize(terminal_runtime_t* runtime, int columns, int rows);
int terminal_runtime_poll(
  terminal_runtime_t* runtime,
  terminal_runtime_output_callback callback,
  void* user_data,
  int* total_shifts
);
int terminal_runtime_exited(terminal_runtime_t* runtime, int* exit_code, int* signal);
int terminal_runtime_close(terminal_runtime_t* runtime);

#ifdef __cplusplus
}
#endif

#endif
