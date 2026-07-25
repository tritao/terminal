#include "../native/emulator/terminal_emulator.h"
#include "../native/runtime/terminal_runtime.h"

#include <stdio.h>
#include <string.h>

#ifndef _WIN32
#include <unistd.h>
#endif

static char output[256];
static size_t output_length;
static int saw_hello;

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
  terminal_emulator_free(emulator);
  if (column != 0 || row != 0)
    return 4;

#ifdef _WIN32
  return 0;
#else
  const char* arguments[] = { "sh", "-c", "printf native-runtime-output", NULL };
  const char* environment[] = { NULL };
  terminal_runtime_t* runtime = terminal_runtime_new(
    80, 24, 100, "xterm-256color", "/bin/sh", arguments, environment, "/tmp"
  );
  if (!runtime)
    return 5;

  int exited = 0;
  for (int i = 0; i < 200 && !strstr(output, "native-runtime-output"); ++i) {
    int shifts = 0;
    terminal_runtime_poll(runtime, collect_output, NULL, &shifts);
    int exit_code, signal;
    exited = terminal_runtime_exited(runtime, &exit_code, &signal);
    if (exited && !strstr(output, "native-runtime-output"))
      break;
    usleep(10000);
  }
  terminal_runtime_close(runtime);
  terminal_runtime_free(runtime);
  return strstr(output, "native-runtime-output") ? 0 : 6;
#endif
}
