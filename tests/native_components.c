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

static void reset_output(void) {
  output_length = 0;
  output[0] = 0;
}

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

static void wait_for_runtime_tick(void) {
#ifdef _WIN32
  Sleep(10);
#else
  usleep(10000);
#endif
}

static int wait_for_runtime(terminal_runtime_t* runtime,
    const char* expected_output, int expected_exit, int timeout_ms,
    int* exit_code, int* signal) {
  int exited = 0;
  int observed_exit = -1;
  int observed_signal = -1;
  int iterations = timeout_ms / 10;
  for (int i = 0; i < iterations; ++i) {
    int shifts = 0;
    terminal_runtime_poll(runtime, collect_output, NULL, &shifts);
    if (!exited)
      exited = terminal_runtime_exited(runtime, &observed_exit,
        &observed_signal);
    if (exited && (!expected_output || strstr(output, expected_output))
        && observed_exit == expected_exit) {
      if (exit_code) *exit_code = observed_exit;
      if (signal) *signal = observed_signal;
      return 1;
    }
    wait_for_runtime_tick();
  }
  if (exit_code) *exit_code = observed_exit;
  if (signal) *signal = observed_signal;
  return 0;
}

static int wait_for_output(terminal_runtime_t* runtime,
    const char* expected_output, int timeout_ms) {
  int iterations = timeout_ms / 10;
  for (int i = 0; i < iterations; ++i) {
    int shifts = 0;
    terminal_runtime_poll(runtime, collect_output, NULL, &shifts);
    if (strstr(output, expected_output))
      return 1;
    wait_for_runtime_tick();
  }
  return 0;
}

static int fail_runtime_test(const char* name, int code) {
  fprintf(stderr, "native runtime test failed: %s (output: %s)\n",
    name, output);
  return code;
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
    "cmd.exe", "/S", "/C", "echo native runtime output", NULL
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

  int exit_code, signal;
  if (!wait_for_runtime(runtime, "native runtime output", 0, 2000,
      &exit_code, &signal)) {
    terminal_runtime_close(runtime);
    terminal_runtime_free(runtime);
    return fail_runtime_test("launch and output", 13);
  }
  terminal_runtime_close(runtime);
  terminal_runtime_free(runtime);

  reset_output();
#ifdef _WIN32
  char windows_cwd[MAX_PATH];
  if (!GetWindowsDirectoryA(windows_cwd, sizeof(windows_cwd)))
    return fail_runtime_test("find Windows working directory", 14);
  const wchar_t windows_environment[] =
    L"PRAGTICAL_TERMINAL_TEST=present\0\0";
  const char* launch_command = "cmd.exe";
  const char* launch_arguments[] = {
    "cmd.exe", "/S", "/C",
    "echo env:%PRAGTICAL_TERMINAL_TEST% & cd & exit /b 7", NULL
  };
  const char* launch_environment[] = {
    (const char*)windows_environment, NULL
  };
  const char* launch_cwd = windows_cwd;
#else
  const char* launch_command = "/bin/sh";
  const char* launch_arguments[] = {
    "sh", "-c",
    "printf 'env:%s cwd:' \"$PRAGTICAL_TERMINAL_TEST\"; pwd; exit 7",
    "sh", NULL
  };
  const char* launch_environment[] = {
    "PRAGTICAL_TERMINAL_TEST", "present", NULL
  };
  const char* launch_cwd = "/tmp";
#endif
  runtime = terminal_runtime_new(80, 24, 100, "xterm-256color",
    launch_command, launch_arguments, launch_environment, launch_cwd);
  if (!runtime)
    return fail_runtime_test("environment and cwd launch", 15);
  if (!wait_for_runtime(runtime, "env:present", 7, 2000,
      &exit_code, &signal)) {
    terminal_runtime_close(runtime);
    terminal_runtime_free(runtime);
    return fail_runtime_test("environment, cwd, and exit status", 16);
  }
#ifdef _WIN32
  if (!strstr(output, windows_cwd)) {
#else
  if (!strstr(output, "cwd:") || !strstr(output, "/tmp")) {
#endif
    terminal_runtime_close(runtime);
    terminal_runtime_free(runtime);
    return fail_runtime_test("working directory output", 17);
  }
  terminal_runtime_close(runtime);
  terminal_runtime_free(runtime);

  reset_output();
#ifdef _WIN32
  const char* input_command = "cmd.exe";
  const char* input_arguments[] = {
    "cmd.exe", "/Q", "/K", "echo ready",
    NULL
  };
#else
  const char* input_command = "/bin/sh";
  const char* input_arguments[] = {
    "sh", "-c",
    "printf 'ready\\n'; read line; printf 'input:%s\\n' \"$line\"; stty size",
    "sh", NULL
  };
#endif
  runtime = terminal_runtime_new(80, 24, 100, "xterm-256color",
    input_command, input_arguments, NULL, NULL);
  if (!runtime)
    return fail_runtime_test("input and resize launch", 18);
  terminal_runtime_resize(runtime, 101, 33);
  if (!wait_for_output(runtime, "ready", 1000)) {
    terminal_runtime_close(runtime);
    terminal_runtime_free(runtime);
    return fail_runtime_test("input readiness", 19);
  }
#ifdef _WIN32
  const char input[] = "echo input:runtime input\r\nmode con\r\nexit\r\n";
#else
  const char input[] = "runtime input\n";
#endif
  if (terminal_runtime_write(runtime, input, strlen(input))
      != (int)strlen(input)
      || !wait_for_runtime(runtime, "input:runtime input", 0, 2000,
        &exit_code, &signal)) {
    terminal_runtime_close(runtime);
    terminal_runtime_free(runtime);
    return fail_runtime_test("input delivery", 20);
  }
#ifdef _WIN32
  if ((!strstr(output, "Lines:")
      && !wait_for_output(runtime, "Lines:", 1000))
      || (!strstr(output, "Columns:")
        && !wait_for_output(runtime, "Columns:", 1000))) {
#else
  if (!strstr(output, "33 101")) {
#endif
    terminal_runtime_close(runtime);
    terminal_runtime_free(runtime);
    return fail_runtime_test("resize delivery", 21);
  }
  terminal_runtime_close(runtime);
  terminal_runtime_free(runtime);

#ifndef _WIN32
  reset_output();
  const char* eof_arguments[] = {
    "sh", "-c", "printf 'ready\\n'; cat; printf 'eof\\n'", "sh", NULL
  };
  runtime = terminal_runtime_new(80, 24, 100, "xterm-256color",
    "/bin/sh", eof_arguments, NULL, NULL);
  if (!runtime)
    return fail_runtime_test("EOF launch", 22);
  if (!wait_for_output(runtime, "ready", 1000)) {
    terminal_runtime_close(runtime);
    terminal_runtime_free(runtime);
    return fail_runtime_test("EOF readiness", 23);
  }
  const char eof = '\004';
  if (terminal_runtime_write(runtime, &eof, 1) != 1
      || !wait_for_runtime(runtime, "eof", 0, 2000, &exit_code, &signal)) {
    terminal_runtime_close(runtime);
    terminal_runtime_free(runtime);
    return fail_runtime_test("EOF delivery", 24);
  }
  terminal_runtime_close(runtime);
  terminal_runtime_free(runtime);

  reset_output();
  const char* interrupt_arguments[] = {
    "sh", "-c",
    "trap 'printf interrupt\\n; exit 0' INT; printf 'ready\\n'; while :; do sleep 1; done",
    "sh", NULL
  };
  runtime = terminal_runtime_new(80, 24, 100, "xterm-256color",
    "/bin/sh", interrupt_arguments, NULL, NULL);
  if (!runtime)
    return fail_runtime_test("interrupt launch", 25);
  if (!wait_for_output(runtime, "ready", 1000)) {
    terminal_runtime_close(runtime);
    terminal_runtime_free(runtime);
    return fail_runtime_test("interrupt readiness", 26);
  }
  const char interrupt = '\003';
  if (terminal_runtime_write(runtime, &interrupt, 1) != 1
      || !wait_for_runtime(runtime, "interrupt", 0, 2000,
        &exit_code, &signal)) {
    terminal_runtime_close(runtime);
    terminal_runtime_free(runtime);
    return fail_runtime_test("interrupt delivery", 27);
  }
  terminal_runtime_close(runtime);
  terminal_runtime_free(runtime);
#endif

  return 0;
}
