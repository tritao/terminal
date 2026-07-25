#include "terminal_emulator.h"
#include "../terminal_core.h"

#include <stdlib.h>

struct terminal_emulator {
  terminal_core_t* core;
};

terminal_emulator_t* terminal_emulator_new(
    int columns, int rows, int scrollback_limit, const char* term) {
  terminal_emulator_t* emulator = calloc(1, sizeof(*emulator));
  if (!emulator)
    return NULL;
  emulator->core = terminal_core_new(columns, rows, scrollback_limit,
    term, "DUMMY", NULL, NULL, NULL);
  if (!emulator->core) {
    free(emulator);
    return NULL;
  }
  return emulator;
}

void terminal_emulator_free(terminal_emulator_t* emulator) {
  if (!emulator)
    return;
  terminal_core_free(emulator->core);
  free(emulator);
}

int terminal_emulator_feed(terminal_emulator_t* emulator,
    const char* data, size_t length) {
  return emulator ? terminal_core_feed(emulator->core, data, (int)length) : 0;
}

void terminal_emulator_resize(terminal_emulator_t* emulator,
    int columns, int rows) {
  if (emulator)
    terminal_core_resize(emulator->core, columns, rows);
}

void terminal_emulator_clear_scrollback(terminal_emulator_t* emulator) {
  if (emulator)
    terminal_core_clear_scrollback(emulator->core);
}

void terminal_emulator_reset(terminal_emulator_t* emulator) {
  if (emulator)
    terminal_core_reset(emulator->core);
}

void terminal_emulator_dimensions(terminal_emulator_t* emulator,
    int* columns, int* rows) {
  if (emulator)
    terminal_core_dimensions(emulator->core, columns, rows);
}

int terminal_emulator_for_each_line(terminal_emulator_t* emulator,
    int first_row, int last_row, terminal_emulator_line_callback callback,
    void* user_data) {
  if (!emulator)
    return 0;
  return terminal_core_for_each_line(emulator->core, first_row, last_row,
    callback, user_data);
}

int terminal_emulator_cursor(terminal_emulator_t* emulator,
    int* column, int* row, int* mode) {
  if (!emulator)
    return 0;
  terminal_core_cursor(emulator->core, column, row, mode);
  return 1;
}

void terminal_emulator_scrollback(terminal_emulator_t* emulator,
    int position, int* current, int* total) {
  if (emulator)
    terminal_core_scrollback(emulator->core, position, current, total);
}
