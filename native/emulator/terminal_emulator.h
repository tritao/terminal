#ifndef PRAGTICAL_TERMINAL_EMULATOR_H
#define PRAGTICAL_TERMINAL_EMULATOR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct terminal_emulator terminal_emulator_t;
typedef void (*terminal_emulator_line_callback)(
  int row,
  uint64_t style,
  const char* text,
  int length,
  int overflow,
  void* user_data
);

terminal_emulator_t* terminal_emulator_new(
  int columns,
  int rows,
  int scrollback_limit,
  const char* term
);
void terminal_emulator_free(terminal_emulator_t* emulator);
int terminal_emulator_feed(terminal_emulator_t* emulator, const char* data, size_t length);
void terminal_emulator_resize(terminal_emulator_t* emulator, int columns, int rows);
void terminal_emulator_clear_scrollback(terminal_emulator_t* emulator);
void terminal_emulator_reset(terminal_emulator_t* emulator);
void terminal_emulator_dimensions(terminal_emulator_t* emulator, int* columns, int* rows);
int terminal_emulator_for_each_line(
  terminal_emulator_t* emulator,
  int first_row,
  int last_row,
  terminal_emulator_line_callback callback,
  void* user_data
);
int terminal_emulator_cursor(
  terminal_emulator_t* emulator,
  int* column,
  int* row,
  int* mode
);
void terminal_emulator_scrollback(
  terminal_emulator_t* emulator,
  int position,
  int* current,
  int* total
);

#ifdef __cplusplus
}
#endif

#endif
