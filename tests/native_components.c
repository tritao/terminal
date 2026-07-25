#include "../native/emulator/terminal_emulator.h"
#include "../native/runtime/terminal_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

static char output[256];
static size_t output_length;
static int saw_hello;
static int saw_scrollback;

static void collect_output(char* data, int length, void* user_data) {
  (void)user_data;
  if (length <= 0 || output_length >= sizeof(output) - 1)
    return;
  size_t available = sizeof(output) - 1 - output_length;
  size_t count = (size_t)length < available ? (size_t)length : available;
  memcpy(&output[output_length], data, count);
  output_length += count;
  output[output_length] = 0;
}

static void inspect_line(int row, uint64_t style, const char* text,
    int length, int overflow, void* user_data) {
  (void)row;
  (void)style;
  (void)overflow;
  (void)user_data;
  for (int i = 0; i + 5 <= length; ++i) {
    if (memcmp(&text[i], "hello", 5) == 0)
      saw_hello = 1;
  }
}

static void inspect_scrollback(int row, uint64_t style, const char* text,
    int length, int overflow, void* user_data) {
  (void)style;
  (void)text;
  (void)length;
  (void)overflow;
  (void)user_data;
  if (row < 0)
    saw_scrollback = 1;
}

int main(void) {
  terminal_emulator_t* emulator = terminal_emulator_new(20, 4, 8, "xterm-256color");
  if (!emulator)
    return 1;
  terminal_emulator_feed(emulator, "hello\r\n", 7);
  terminal_emulator_for_each_line(emulator, 0, 3, inspect_line, NULL);
  if (!saw_hello) {
    terminal_emulator_free(emulator);
    return 2;
  }
  int column, row, mode;
  if (!terminal_emulator_cursor(emulator, &column, &row, &mode)
      || column != 0 || row != 1) {
    terminal_emulator_free(emulator);
    return 3;
  }
  terminal_emulator_reset(emulator);
  terminal_emulator_cursor(emulator, &column, &row, &mode);
  if (column != 0 || row != 0)
    return 4;

  const char* lines = "one\r\ntwo\r\nthree\r\nfour\r\nfive\r\n";
  terminal_emulator_feed(emulator, lines, strlen(lines));
  int scrollback_position, scrollback_total;
  terminal_emulator_scrollback(emulator, 1, &scrollback_position, &scrollback_total);
  terminal_emulator_for_each_line(emulator, -1, 2, inspect_scrollback, NULL);
  if (scrollback_position != 1 || scrollback_total < 1 || !saw_scrollback)
    return 5;
  terminal_emulator_feed(emulator, "\x1B[?1006h", 8);
  const char* title_sequence = "\x1B]0;checkpoint-title\a";
  terminal_emulator_feed(emulator, title_sequence, strlen(title_sequence));
  int source_mouse_encoding;
  terminal_emulator_modes(emulator, NULL, NULL, NULL, &source_mouse_encoding, NULL, NULL);
  const char* source_name = terminal_emulator_name(emulator);
  if (source_mouse_encoding != 1
      || !source_name || strcmp(source_name, "checkpoint-title") != 0)
    return 6;

  size_t checkpoint_size = terminal_emulator_checkpoint_size(emulator);
  void* checkpoint = malloc(checkpoint_size);
  size_t checkpoint_written = 0;
  if (!checkpoint_size || !checkpoint
      || !terminal_emulator_checkpoint(emulator, checkpoint, checkpoint_size,
        &checkpoint_written)
      || checkpoint_written != checkpoint_size) {
    free(checkpoint);
    terminal_emulator_free(emulator);
    return 7;
  }
  terminal_emulator_t* restored = terminal_emulator_new(5, 2, 2, "xterm");
  if (!restored || !terminal_emulator_restore_checkpoint(
      restored, checkpoint, checkpoint_written)) {
    free(checkpoint);
    terminal_emulator_free(restored);
    terminal_emulator_free(emulator);
    return 8;
  }
  if (terminal_emulator_restore_checkpoint(restored, checkpoint,
      checkpoint_written - 1)) {
    free(checkpoint);
    terminal_emulator_free(restored);
    terminal_emulator_free(emulator);
    return 9;
  }
  int restored_columns, restored_rows;
  int restored_mouse_encoding;
  terminal_emulator_dimensions(restored, &restored_columns, &restored_rows);
  terminal_emulator_cursor(restored, &column, &row, &mode);
  terminal_emulator_modes(restored, NULL, NULL, NULL, &restored_mouse_encoding, NULL, NULL);
  terminal_emulator_scrollback(restored, -1, &scrollback_position, &scrollback_total);
  saw_scrollback = 0;
  terminal_emulator_for_each_line(restored, -1, 2, inspect_scrollback, NULL);
  if (restored_columns != 20 || restored_rows != 4
      || column != 0 || row != 3
      || restored_mouse_encoding != 1
      || !terminal_emulator_name(restored)
      || strcmp(terminal_emulator_name(restored), "checkpoint-title") != 0
      || scrollback_position != 1 || scrollback_total < 1 || !saw_scrollback) {
    free(checkpoint);
    terminal_emulator_free(restored);
    terminal_emulator_free(emulator);
    return 10;
  }
  free(checkpoint);
  terminal_emulator_free(restored);

  terminal_emulator_free(emulator);

  if (terminal_runtime_new(0, 24, 100, "xterm-256color", "/bin/sh",
      NULL, NULL, NULL))
    return 11;
#ifdef _WIN32
  const char* command = "cmd.exe";
  const char* arguments[] = {
    "cmd.exe", "/C", "echo native runtime output", NULL
  };
  const char* cwd = NULL;
#else
  const char* command = "/bin/sh";
  const char* arguments[] = {
    "sh", "-c", "printf '%s' \"$1\"", "sh", "native runtime output", NULL
  };
  const char* cwd = "/tmp";
#endif
  terminal_runtime_t* runtime = terminal_runtime_new(
    80, 24, 100, "xterm-256color", command, arguments, NULL, cwd
  );
  if (!runtime)
    return 12;

  int exited = 0;
  for (int i = 0; i < 200 && !strstr(output, "native runtime output"); ++i) {
    int shifts = 0;
    terminal_runtime_poll(runtime, collect_output, NULL, &shifts);
    int exit_code, signal;
    exited = terminal_runtime_exited(runtime, &exit_code, &signal);
    if (exited && i > 20 && !strstr(output, "native runtime output"))
      break;
#ifdef _WIN32
    Sleep(10);
#else
    usleep(10000);
#endif
  }
  terminal_runtime_close(runtime);
  terminal_runtime_free(runtime);
  return strstr(output, "native runtime output") ? 0 : 13;
}
