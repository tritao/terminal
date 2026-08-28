#include "terminal_core.h"
#include "emulator/terminal_emulator.h"
#include "runtime/terminal_runtime.h"

#include <stdlib.h>
#include <string.h>

struct terminal_core {
  terminal_emulator_t* emulator;
  terminal_runtime_t* runtime;
  int closed;
  char encoded_input[256];
  int encoded_input_length;
};

static void emulator_input_callback(const char* data, int length,
    void* user_data) {
  terminal_core_t* terminal = (terminal_core_t*)user_data;
  if (terminal->runtime)
    terminal_runtime_write(terminal->runtime, data, (size_t)length);
  else if (length > 0) {
    int available = (int)sizeof(terminal->encoded_input)
      - terminal->encoded_input_length;
    if (length > available)
      length = available;
    if (length > 0) {
      memcpy(&terminal->encoded_input[terminal->encoded_input_length], data,
        (size_t)length);
      terminal->encoded_input_length += length;
    }
  }
}

terminal_core_t* terminal_core_new(
    int columns, int rows, int scrollback_limit, const char* term_env,
    const char* command, const char** arguments, const char** environment,
    const char* cwd) {
  terminal_core_t* terminal = calloc(1, sizeof(*terminal));
  if (!terminal)
    return NULL;

  terminal->emulator = terminal_emulator_new(
    columns, rows, scrollback_limit, term_env);
  if (!terminal->emulator) {
    free(terminal);
    return NULL;
  }

  if (command && strcmp(command, "DUMMY") != 0) {
    terminal->runtime = terminal_runtime_new(
      columns, rows, scrollback_limit, term_env, command, arguments,
      environment, cwd);
    if (!terminal->runtime) {
      terminal_emulator_free(terminal->emulator);
      free(terminal);
      return NULL;
    }
  }
  terminal_emulator_set_input_callback(
    terminal->emulator, emulator_input_callback, terminal);
  return terminal;
}

void terminal_core_free(terminal_core_t* terminal) {
  if (!terminal)
    return;
  terminal_runtime_free(terminal->runtime);
  terminal_emulator_free(terminal->emulator);
  free(terminal);
}

const char* terminal_core_last_error(void) {
  return terminal_runtime_last_error();
}

int terminal_core_close(terminal_core_t* terminal) {
  if (!terminal || terminal->closed)
    return 0;
  terminal->closed = 1;
  int result = terminal_runtime_close(terminal->runtime);
  terminal_emulator_close(terminal->emulator);
  return result;
}

void terminal_core_set_debug(terminal_core_t* terminal, int enabled) {
  if (terminal)
    terminal_emulator_set_debug(terminal->emulator, enabled);
}

int terminal_core_feed(terminal_core_t* terminal, const char* data, int length) {
  if (!terminal || terminal->closed)
    return 0;
  if (terminal->runtime)
    return terminal_runtime_write(terminal->runtime, data, (size_t)length);
  return terminal_emulator_feed(terminal->emulator, data, (size_t)length);
}

size_t terminal_core_checkpoint_size(terminal_core_t* terminal) {
  return terminal && !terminal->closed
    ? terminal_emulator_checkpoint_size(terminal->emulator) : 0;
}

int terminal_core_checkpoint(terminal_core_t* terminal, void* data,
    size_t size, size_t* written) {
  return terminal && !terminal->closed
    ? terminal_emulator_checkpoint(terminal->emulator, data, size, written) : 0;
}

int terminal_core_restore_checkpoint(terminal_core_t* terminal,
    const void* data, size_t size) {
  return terminal && !terminal->closed
    ? terminal_emulator_restore_checkpoint(terminal->emulator, data, size) : 0;
}

void terminal_core_clear_scrollback(terminal_core_t* terminal) {
  if (terminal)
    terminal_emulator_clear_scrollback(terminal->emulator);
}

void terminal_core_clear(terminal_core_t* terminal) {
  if (terminal)
    terminal_emulator_clear(terminal->emulator);
}

void terminal_core_reset(terminal_core_t* terminal) {
  if (terminal)
    terminal_emulator_reset(terminal->emulator);
}

typedef struct terminal_core_poll_context {
  terminal_core_t* terminal;
  terminal_core_output_callback callback;
  void* user_data;
  int total_shifts;
} terminal_core_poll_context_t;

static void terminal_core_output(char* data, int length, void* user_data) {
  terminal_core_poll_context_t* context =
    (terminal_core_poll_context_t*)user_data;
  context->total_shifts += terminal_emulator_feed(
    context->terminal->emulator, data, (size_t)length);
  if (context->callback)
    context->callback(data, length, context->user_data);
}

int terminal_core_update(terminal_core_t* terminal,
    terminal_core_output_callback callback, void* user_data,
    int* total_shifts) {
  if (!terminal || terminal->closed || !terminal->runtime)
    return 0;
  terminal_core_poll_context_t context = {
    .terminal = terminal,
    .callback = callback,
    .user_data = user_data,
  };
  int status = terminal_runtime_poll(terminal->runtime, terminal_core_output,
    &context, &context.total_shifts);
  if (total_shifts)
    *total_shifts = context.total_shifts;
  return status;
}

void terminal_core_resize(terminal_core_t* terminal, int columns, int rows) {
  if (!terminal || terminal->closed)
    return;
  terminal_emulator_resize(terminal->emulator, columns, rows);
  terminal_runtime_resize(terminal->runtime, columns, rows);
}

int terminal_core_is_closed(terminal_core_t* terminal) {
  return !terminal || terminal->closed;
}

void terminal_core_dimensions(terminal_core_t* terminal, int* columns, int* rows) {
  if (terminal)
    terminal_emulator_dimensions(terminal->emulator, columns, rows);
}

void terminal_core_cursor(terminal_core_t* terminal, int* column, int* row, int* mode) {
  if (terminal)
    terminal_emulator_cursor(terminal->emulator, column, row, mode);
}

void terminal_core_modes(terminal_core_t* terminal, int* cursor_keys_mode,
    int* keypad_keys_mode, int* mouse_tracking_mode, int* mouse_encoding,
    int* paste_mode, int* reporting_focus) {
  if (terminal)
    terminal_emulator_modes(terminal->emulator, cursor_keys_mode,
      keypad_keys_mode, mouse_tracking_mode, mouse_encoding, paste_mode,
      reporting_focus);
}

const char* terminal_core_name(terminal_core_t* terminal) {
  return terminal ? terminal_emulator_name(terminal->emulator) : NULL;
}

void terminal_core_focus(terminal_core_t* terminal, int focused) {
  if (terminal)
    terminal_emulator_focus(terminal->emulator, focused);
}

void terminal_core_scrollback(terminal_core_t* terminal, int position,
    int* current, int* total) {
  if (terminal)
    terminal_emulator_scrollback(terminal->emulator, position, current, total);
}

int terminal_core_for_each_line(terminal_core_t* terminal, int first_row,
    int last_row, terminal_core_line_callback callback, void* user_data) {
  return terminal && !terminal->closed
    ? terminal_emulator_for_each_line(terminal->emulator, first_row, last_row,
      callback, user_data)
    : 0;
}

int terminal_core_synchronized_output(terminal_core_t* terminal) {
  return terminal && !terminal->closed
    ? terminal_emulator_synchronized_output(terminal->emulator) : 0;
}

int terminal_core_keyboard(terminal_core_t* terminal, const char* key_name,
    unsigned int modifiers, uint32_t unicode) {
  if (terminal)
    terminal->encoded_input_length = 0;
  return terminal && !terminal->closed
    ? terminal_emulator_keyboard(terminal->emulator, key_name, modifiers,
      unicode) : 0;
}

const char* terminal_core_encoded_input(terminal_core_t* terminal, int* length) {
  if (length)
    *length = terminal ? terminal->encoded_input_length : 0;
  return terminal ? terminal->encoded_input : NULL;
}

int terminal_core_mouse(terminal_core_t* terminal, unsigned int cell_x,
    unsigned int cell_y, unsigned int button, unsigned int event,
    unsigned char modifiers) {
  return terminal && !terminal->closed
    ? terminal_emulator_mouse(terminal->emulator, cell_x, cell_y, button,
      event, modifiers) : 0;
}

int terminal_core_exited(terminal_core_t* terminal, int* exit_code, int* signal) {
  return terminal && terminal->runtime
    ? terminal_runtime_exited(terminal->runtime, exit_code, signal)
    : 0;
}
