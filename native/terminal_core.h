#ifndef PRAGTICAL_TERMINAL_CORE_H
#define PRAGTICAL_TERMINAL_CORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct terminal_core terminal_core_t;
typedef void (*terminal_core_output_callback)(char* data, int length, void* user_data);
typedef void (*terminal_core_line_callback)(
  int row,
  uint64_t style,
  const char* text,
  int length,
  int overflow,
  void* user_data
);

terminal_core_t* terminal_core_new(
  int columns,
  int rows,
  int scrollback_limit,
  const char* term,
  const char* command,
  const char** arguments,
  const char** environment,
  const char* cwd
);

void terminal_core_free(terminal_core_t* terminal);
int terminal_core_close(terminal_core_t* terminal);
int terminal_core_feed(terminal_core_t* terminal, const char* data, int length);
void terminal_core_clear_scrollback(terminal_core_t* terminal);
void terminal_core_reset(terminal_core_t* terminal);
int terminal_core_update(
  terminal_core_t* terminal,
  terminal_core_output_callback callback,
  void* user_data,
  int* total_shifts
);
void terminal_core_resize(terminal_core_t* terminal, int columns, int rows);
int terminal_core_is_closed(terminal_core_t* terminal);
void terminal_core_dimensions(terminal_core_t* terminal, int* columns, int* rows);
void terminal_core_cursor(terminal_core_t* terminal, int* column, int* row, int* mode);
void terminal_core_scrollback(terminal_core_t* terminal, int position, int* current, int* total);
int terminal_core_for_each_line(
  terminal_core_t* terminal,
  int first_row,
  int last_row,
  terminal_core_line_callback callback,
  void* user_data
);
int terminal_core_exited(terminal_core_t* terminal, int* exit_code, int* signal);

#ifdef __cplusplus
}
#endif

#endif
