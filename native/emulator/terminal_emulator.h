#ifndef PRAGTICAL_TERMINAL_EMULATOR_H
#define PRAGTICAL_TERMINAL_EMULATOR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct terminal_emulator terminal_emulator_t;
typedef void (*terminal_emulator_input_callback)(
  const char* data,
  int length,
  void* user_data
);
typedef void (*terminal_emulator_line_callback)(
  int row,
  int column,
  int cells,
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
void terminal_emulator_set_debug(terminal_emulator_t* emulator, int enabled);
void terminal_emulator_set_input_callback(
  terminal_emulator_t* emulator,
  terminal_emulator_input_callback callback,
  void* user_data
);
int terminal_emulator_feed(
  terminal_emulator_t* emulator,
  const char* data,
  size_t length
);
/* Checkpoints are versioned, little-endian emulator state snapshots. */
size_t terminal_emulator_checkpoint_size(terminal_emulator_t* emulator);
int terminal_emulator_checkpoint(
  terminal_emulator_t* emulator,
  void* data,
  size_t size,
  size_t* written
);
int terminal_emulator_restore_checkpoint(
  terminal_emulator_t* emulator,
  const void* data,
  size_t size
);
void terminal_emulator_resize(terminal_emulator_t* emulator, int columns, int rows);
int terminal_emulator_close(terminal_emulator_t* emulator);
int terminal_emulator_is_closed(terminal_emulator_t* emulator);
void terminal_emulator_clear(terminal_emulator_t* emulator);
void terminal_emulator_clear_scrollback(terminal_emulator_t* emulator);
void terminal_emulator_reset(terminal_emulator_t* emulator);
void terminal_emulator_dimensions(terminal_emulator_t* emulator, int* columns, int* rows);
int terminal_emulator_cursor(
  terminal_emulator_t* emulator,
  int* column,
  int* row,
  int* mode
);
void terminal_emulator_modes(
  terminal_emulator_t* emulator,
  int* cursor_keys_mode,
  int* keypad_keys_mode,
  int* mouse_tracking_mode,
  int* mouse_encoding,
  int* paste_mode,
  int* reporting_focus
);
const char* terminal_emulator_name(terminal_emulator_t* emulator);
void terminal_emulator_focus(terminal_emulator_t* emulator, int focused);
void terminal_emulator_scrollback(
  terminal_emulator_t* emulator,
  int position,
  int* current,
  int* total
);
int terminal_emulator_for_each_line(
  terminal_emulator_t* emulator,
  int first_row,
  int last_row,
  terminal_emulator_line_callback callback,
  void* user_data
);
int terminal_emulator_synchronized_output(terminal_emulator_t* emulator);
/* Modifier bits: shift=1, lock=2, control=4, alt=8, logo=16. */
int terminal_emulator_keyboard(terminal_emulator_t* emulator,
  const char* key_name, unsigned int modifiers, uint32_t unicode);
int terminal_emulator_mouse(
  terminal_emulator_t* emulator,
  unsigned int cell_x,
  unsigned int cell_y,
  unsigned int button,
  unsigned int event,
  unsigned char modifiers
);

#ifdef __cplusplus
}
#endif

#endif
