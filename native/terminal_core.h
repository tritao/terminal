#ifndef PRAGTICAL_TERMINAL_CORE_H
#define PRAGTICAL_TERMINAL_CORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct terminal_core terminal_core_t;

typedef enum terminal_core_cursor_mode {
  TERMINAL_CORE_CURSOR_SOLID = 0,
  TERMINAL_CORE_CURSOR_HIDDEN = 1,
  TERMINAL_CORE_CURSOR_BLINKING = 2
} terminal_core_cursor_mode_t;

typedef enum terminal_core_keys_mode {
  TERMINAL_CORE_KEYS_NORMAL = 0,
  TERMINAL_CORE_KEYS_APPLICATION = 1
} terminal_core_keys_mode_t;

typedef enum terminal_core_mouse_tracking_mode {
  TERMINAL_CORE_MOUSE_NONE = 0,
  TERMINAL_CORE_MOUSE_X10 = 1,
  TERMINAL_CORE_MOUSE_NORMAL = 2,
  TERMINAL_CORE_MOUSE_BUTTON = 3,
  TERMINAL_CORE_MOUSE_ANY = 4
} terminal_core_mouse_tracking_mode_t;

typedef enum terminal_core_mouse_encoding {
  TERMINAL_CORE_MOUSE_ENCODING_DEFAULT = 0,
  TERMINAL_CORE_MOUSE_ENCODING_SGR = 1
} terminal_core_mouse_encoding_t;

typedef enum terminal_core_paste_mode {
  TERMINAL_CORE_PASTE_NORMAL = 0,
  TERMINAL_CORE_PASTE_BRACKETED = 1
} terminal_core_paste_mode_t;
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
const char* terminal_core_last_error(void);
int terminal_core_close(terminal_core_t* terminal);
void terminal_core_set_debug(terminal_core_t* terminal, int enabled);
int terminal_core_feed(terminal_core_t* terminal, const char* data, int length);
size_t terminal_core_checkpoint_size(terminal_core_t* terminal);
int terminal_core_checkpoint(
  terminal_core_t* terminal,
  void* data,
  size_t size,
  size_t* written
);
int terminal_core_restore_checkpoint(
  terminal_core_t* terminal,
  const void* data,
  size_t size
);
void terminal_core_clear_scrollback(terminal_core_t* terminal);
void terminal_core_clear(terminal_core_t* terminal);
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
void terminal_core_modes(
  terminal_core_t* terminal,
  int* cursor_keys_mode,
  int* keypad_keys_mode,
  int* mouse_tracking_mode,
  int* mouse_encoding,
  int* paste_mode,
  int* reporting_focus
);
const char* terminal_core_name(terminal_core_t* terminal);
void terminal_core_focus(terminal_core_t* terminal, int focused);
void terminal_core_scrollback(terminal_core_t* terminal, int position, int* current, int* total);
int terminal_core_for_each_line(
  terminal_core_t* terminal,
  int first_row,
  int last_row,
  terminal_core_line_callback callback,
  void* user_data
);
int terminal_core_synchronized_output(terminal_core_t* terminal);
int terminal_core_mouse(terminal_core_t* terminal, unsigned int cell_x,
    unsigned int cell_y, unsigned int button, unsigned int event,
    unsigned char modifiers);
int terminal_core_exited(terminal_core_t* terminal, int* exit_code, int* signal);

#ifdef __cplusplus
}
#endif

#endif
