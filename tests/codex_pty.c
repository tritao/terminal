#include "emulator/terminal_emulator.h"
#include "runtime/terminal_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

static const char probe[] = "\x1B[?u";
static const char keyboard_push[] = "\x1B[>7u";
static const char probe_response[] = "\x1B[?7u";
static const char prompt[] = "Start and wait for further input. Do not modify files.";

typedef struct codex_pty_test {
  terminal_runtime_t* runtime;
  terminal_emulator_t* emulator;
  char output[256 * 1024];
  size_t output_length;
  char responses[16 * 1024];
  size_t response_length;
} codex_pty_test_t;

static void append_bytes(char* destination, size_t capacity,
    size_t* length, const char* data, int data_length) {
  if (!data || data_length <= 0 || *length >= capacity - 1)
    return;
  size_t available = capacity - 1 - *length;
  size_t count = (size_t)data_length < available
    ? (size_t)data_length : available;
  memcpy(destination + *length, data, count);
  *length += count;
  destination[*length] = 0;
}

static int contains_bytes(const char* data, size_t data_length,
    const char* needle, size_t needle_length) {
  if (!data || !needle || !needle_length || needle_length > data_length)
    return 0;
  for (size_t i = 0; i + needle_length <= data_length; i++) {
    if (!memcmp(data + i, needle, needle_length))
      return 1;
  }
  return 0;
}

static void sleep_milliseconds(unsigned int milliseconds) {
#ifdef _WIN32
  Sleep(milliseconds);
#else
  usleep(milliseconds * 1000);
#endif
}

static void collect_emulator_output(const char* data, int length,
    void* user_data) {
  codex_pty_test_t* test = user_data;
  append_bytes(test->responses, sizeof(test->responses),
    &test->response_length, data, length);
  terminal_runtime_write(test->runtime, data, (size_t)length);
}

static void collect_runtime_output(char* data, int length, void* user_data) {
  codex_pty_test_t* test = user_data;
  append_bytes(test->output, sizeof(test->output),
    &test->output_length, data, length);
  terminal_emulator_feed(test->emulator, data, (size_t)length);
}

int main(void) {
  const char* command = getenv("CODEX_BIN");
  if (!command || !*command)
    command = "codex";

  const char* arguments[] = {
    command,
    "--no-alt-screen",
    "--sandbox", "read-only",
    "--ask-for-approval", "never",
    prompt,
    NULL
  };

  codex_pty_test_t test = {0};
  test.runtime = terminal_runtime_new(
    115, 15, 200, "xterm-256color", command, arguments, NULL, NULL
  );
  if (!test.runtime) {
    fprintf(stderr, "codex-pty: unable to start '%s': %s\n",
      command, terminal_runtime_last_error());
    return 77;
  }

  test.emulator = terminal_emulator_new(115, 15, 200, "xterm-256color");
  if (!test.emulator) {
    terminal_runtime_close(test.runtime);
    terminal_runtime_free(test.runtime);
    return 1;
  }
  terminal_emulator_set_input_callback(test.emulator,
    collect_emulator_output, &test);

  int exited = 0;
  int exit_code = -1;
  int signal = -1;
  const char* timeout_value = getenv("CODEX_PTY_TIMEOUT_MS");
  unsigned int timeout_ms = 120000;
  if (timeout_value && *timeout_value) {
    char* end = NULL;
    unsigned long parsed = strtoul(timeout_value, &end, 10);
    if (end != timeout_value && *end == 0 && parsed > 0
        && parsed <= 600000)
      timeout_ms = (unsigned int)parsed;
  }
  for (unsigned int elapsed = 0; elapsed < timeout_ms; elapsed += 20) {
    int shifts = 0;
    terminal_runtime_poll(test.runtime, collect_runtime_output, &test, &shifts);
    if (!exited)
      exited = terminal_runtime_exited(test.runtime, &exit_code, &signal);

    if (contains_bytes(test.output, test.output_length,
          probe, sizeof(probe) - 1)
        && contains_bytes(test.output, test.output_length,
          keyboard_push, sizeof(keyboard_push) - 1)
        && contains_bytes(test.responses, test.response_length,
          probe_response, sizeof(probe_response) - 1))
      break;
    if (exited)
      break;
    sleep_milliseconds(20);
  }

  int saw_probe = contains_bytes(test.output, test.output_length,
    probe, sizeof(probe) - 1);
  int saw_push = contains_bytes(test.output, test.output_length,
    keyboard_push, sizeof(keyboard_push) - 1);
  int saw_response = contains_bytes(test.responses, test.response_length,
    probe_response, sizeof(probe_response) - 1);

  if (!saw_probe || !saw_push || !saw_response) {
    fprintf(stderr,
      "codex-pty failed: probe=%d push=%d response=%d exited=%d "
      "exit_code=%d signal=%d output_bytes=%zu response_bytes=%zu\n",
      saw_probe, saw_push, saw_response, exited, exit_code, signal,
      test.output_length, test.response_length);
    if (test.response_length) {
      fprintf(stderr, "codex-pty responses:");
      for (size_t i = 0; i < test.response_length; ++i)
        fprintf(stderr, " %02x", (unsigned char)test.responses[i]);
      fputc('\n', stderr);
    }
    terminal_runtime_close(test.runtime);
    terminal_emulator_free(test.emulator);
    terminal_runtime_free(test.runtime);
    return 1;
  }

  printf("codex-pty ok: Codex Kitty probe negotiated through libtsm\n");
  terminal_runtime_close(test.runtime);
  terminal_emulator_free(test.emulator);
  terminal_runtime_free(test.runtime);
  return 0;
}
