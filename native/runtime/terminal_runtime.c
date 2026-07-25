#if _WIN32
  #if __MINGW32__ || __MINGW64__
    #define NTDDI_VERSION 0x0A000006
    #undef _WIN32_WINNT
    #define _WIN32_WINNT 0x0A00
  #endif
  #include <windows.h>
  #include <wincon.h>
#else
  #include <errno.h>
  #include <fcntl.h>
  #include <signal.h>
  #include <sys/ioctl.h>
  #include <sys/types.h>
  #include <sys/wait.h>
  #if __APPLE__
    #include <util.h>
  #else
    #include <pty.h>
  #endif
  #include <unistd.h>
#endif

#include "terminal_runtime.h"

#include <stdio.h>
#include <stdint.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define TERMINAL_RUNTIME_CHUNK_SIZE 4096
#define TERMINAL_RUNTIME_MAX_CHUNKS 10

struct terminal_runtime {
  int closed;
#if _WIN32
  PROCESS_INFORMATION process_information;
  HPCON hpcon;
  HANDLE topty;
  HANDLE frompty;
  char nonblocking_buffer[TERMINAL_RUNTIME_CHUNK_SIZE];
  int nonblocking_buffer_length;
  HANDLE nonblocking_buffer_mutex;
  HANDLE nonblocking_thread;
  int closing;
#else
  int master;
  pid_t pid;
#endif
};

static char error_step[64];
#if _WIN32
static long long last_error_code;
#endif

static int set_error_step(const char* step) {
  strncpy(error_step, step, sizeof(error_step) - 1);
  error_step[sizeof(error_step) - 1] = 0;
  return 1;
}

const char* terminal_runtime_last_error(void) {
#if _WIN32
  static char error_buffer[2048];
  strcpy(error_buffer, error_step);
  int length = (int)strlen(error_buffer);
  error_buffer[length++] = ':';
  error_buffer[length++] = ' ';
  FormatMessageA(
    FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
    NULL,
    last_error_code,
    MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
    (LPSTR)&error_buffer[length],
    sizeof(error_buffer) - (size_t)length - 1,
    NULL
  );
  return error_buffer;
#else
  return error_step;
#endif
}

#if _WIN32
#define TERMINAL_RUNTIME_MAX_ARGUMENTS 256

static wchar_t* windows_utf8_to_wide(const char* value) {
  if (!value)
    return NULL;
  int length = MultiByteToWideChar(CP_UTF8, 0, value, -1, NULL, 0);
  if (length <= 0)
    return NULL;
  wchar_t* result = malloc(sizeof(*result) * (size_t)length);
  if (!result)
    return NULL;
  if (!MultiByteToWideChar(CP_UTF8, 0, value, -1, result, length)) {
    free(result);
    return NULL;
  }
  return result;
}

static int windows_size_add(size_t* value, size_t addition) {
  if (addition > SIZE_MAX - *value)
    return 0;
  *value += addition;
  return 1;
}

/* Quote one argument using the CommandLineToArgvW/CRT command-line rules. */
static int windows_quoted_argument_length(const wchar_t* argument,
    size_t* length) {
  size_t result = 2;
  const wchar_t* cursor = argument;
  while (*cursor) {
    size_t backslashes = 0;
    while (cursor[backslashes] == L'\\')
      backslashes++;
    if (cursor[backslashes] == L'\0') {
      if (backslashes > (SIZE_MAX - result) / 2)
        return 0;
      result += backslashes * 2;
      break;
    }
    if (cursor[backslashes] == L'"') {
      if (backslashes > (SIZE_MAX - result - 2) / 2)
        return 0;
      result += backslashes * 2 + 2;
      cursor += backslashes + 1;
    } else {
      if (!windows_size_add(&result, backslashes + 1))
        return 0;
      cursor += backslashes + 1;
    }
  }
  *length = result;
  return 1;
}

static wchar_t* windows_append_quoted_argument(wchar_t* output,
    const wchar_t* argument) {
  *output++ = L'"';
  const wchar_t* cursor = argument;
  while (*cursor) {
    size_t backslashes = 0;
    while (cursor[backslashes] == L'\\')
      backslashes++;
    if (cursor[backslashes] == L'\0') {
      for (size_t i = 0; i < backslashes * 2; ++i)
        *output++ = L'\\';
      cursor += backslashes;
      break;
    }
    if (cursor[backslashes] == L'"') {
      for (size_t i = 0; i < backslashes * 2 + 1; ++i)
        *output++ = L'\\';
      *output++ = L'"';
      cursor += backslashes + 1;
    } else {
      for (size_t i = 0; i < backslashes; ++i)
        *output++ = L'\\';
      *output++ = cursor[backslashes];
      cursor += backslashes + 1;
    }
  }
  *output++ = L'"';
  return output;
}

static wchar_t* windows_build_command_line(const char* command,
    const char** arguments) {
  const char* values[TERMINAL_RUNTIME_MAX_ARGUMENTS] = {0};
  wchar_t* wide_values[TERMINAL_RUNTIME_MAX_ARGUMENTS] = {0};
  int value_count = 1;
  values[0] = command;
  if (arguments) {
    for (int i = 1; i < TERMINAL_RUNTIME_MAX_ARGUMENTS && arguments[i]; ++i)
      values[value_count++] = arguments[i];
    if (value_count == TERMINAL_RUNTIME_MAX_ARGUMENTS
        && arguments[TERMINAL_RUNTIME_MAX_ARGUMENTS - 1]) {
      set_error_step("too many command arguments");
      return NULL;
    }
  }

  size_t command_line_length = 0;
  for (int i = 0; i < value_count; ++i) {
    wide_values[i] = windows_utf8_to_wide(values[i]);
    if (!wide_values[i]) {
      set_error_step("convert command line to UTF-16");
      goto error;
    }
    size_t argument_length;
    if (!windows_quoted_argument_length(wide_values[i], &argument_length)
        || !windows_size_add(&command_line_length, argument_length)
        || (i + 1 < value_count
          && !windows_size_add(&command_line_length, 1))) {
      set_error_step("size command line");
      goto error;
    }
  }
  if (command_line_length == SIZE_MAX
      || command_line_length + 1 > SIZE_MAX / sizeof(wchar_t)) {
    set_error_step("allocate command line");
    goto error;
  }

  wchar_t* command_line = malloc(
    sizeof(*command_line) * (command_line_length + 1));
  if (!command_line) {
    set_error_step("allocate command line");
    goto error;
  }
  wchar_t* output = command_line;
  for (int i = 0; i < value_count; ++i) {
    if (i)
      *output++ = L' ';
    output = windows_append_quoted_argument(output, wide_values[i]);
  }
  *output = L'\0';
  for (int i = 0; i < value_count; ++i)
    free(wide_values[i]);
  return command_line;

error:
  for (int i = 0; i < value_count; ++i)
    free(wide_values[i]);
  return NULL;
}

static DWORD windows_nonblocking_thread_callback(void* data) {
  terminal_runtime_t* runtime = (terminal_runtime_t*)data;
  char chunk[TERMINAL_RUNTIME_CHUNK_SIZE];
  while (!runtime->closing) {
    WaitForSingleObject(runtime->nonblocking_buffer_mutex, INFINITE);
    int available = (int)sizeof(runtime->nonblocking_buffer)
      - runtime->nonblocking_buffer_length;
    ReleaseMutex(runtime->nonblocking_buffer_mutex);
    if (runtime->closing)
      break;
    if (available <= 0) {
      Sleep(1);
      continue;
    }

    DWORD bytes_read = 0;
    DWORD read_size = (DWORD)available;
    if (read_size > sizeof(chunk))
      read_size = sizeof(chunk);
    if (!ReadFile(runtime->frompty, chunk, read_size, &bytes_read, NULL))
      break;
    if (bytes_read > 0) {
      WaitForSingleObject(runtime->nonblocking_buffer_mutex, INFINITE);
      if (!runtime->closing) {
        memcpy(&runtime->nonblocking_buffer[runtime->nonblocking_buffer_length],
          chunk, bytes_read);
        runtime->nonblocking_buffer_length += (int)bytes_read;
      }
      ReleaseMutex(runtime->nonblocking_buffer_mutex);
    }
    Sleep(1);
  }
  return 0;
}
#endif

terminal_runtime_t* terminal_runtime_new(
    int columns, int rows, int scrollback_limit, const char* term,
    const char* command, const char** arguments, const char** environment,
    const char* cwd) {
  (void)scrollback_limit;
  if (!command || !*command || columns <= 0 || rows <= 0
#if _WIN32
      || columns > SHRT_MAX || rows > SHRT_MAX
#else
      || columns > USHRT_MAX || rows > USHRT_MAX
#endif
  ) {
    set_error_step("invalid runtime options");
#if _WIN32
    last_error_code = ERROR_INVALID_PARAMETER;
#endif
    return NULL;
  }
  terminal_runtime_t* runtime = calloc(1, sizeof(*runtime));
  if (!runtime)
    return NULL;

#if _WIN32
  last_error_code = 0;
  HRESULT result = S_OK;
  SECURITY_ATTRIBUTES no_sec = {
    .nLength = sizeof(SECURITY_ATTRIBUTES),
    .bInheritHandle = TRUE,
    .lpSecurityDescriptor = NULL
  };
  HANDLE out_pipe_pseudo_console_side = NULL;
  HANDLE in_pipe_pseudo_console_side = NULL;
  wchar_t* commandline = NULL;
  wchar_t* working_directory = NULL;
  STARTUPINFOEXW startup = {0};
  BOOL attribute_list_initialized = FALSE;
  COORD size = { columns, rows };
  if (!CreatePipe(&in_pipe_pseudo_console_side, &runtime->topty, &no_sec, 0)
      || !CreatePipe(&runtime->frompty, &out_pipe_pseudo_console_side, &no_sec, 0)) {
    set_error_step("create pipes");
    last_error_code = GetLastError();
    goto error;
  }
  if (!SetHandleInformation(runtime->topty, HANDLE_FLAG_INHERIT, 0)
      || !SetHandleInformation(runtime->frompty, HANDLE_FLAG_INHERIT, 0)) {
    set_error_step("configure pipe inheritance");
    last_error_code = GetLastError();
    goto error;
  }
  result = CreatePseudoConsole(size, in_pipe_pseudo_console_side,
    out_pipe_pseudo_console_side, 0, &runtime->hpcon);
  if (FAILED(result)) {
    set_error_step("create pseudoconsole");
    last_error_code = HRESULT_CODE(result);
    goto error;
  }
  runtime->nonblocking_buffer_mutex = CreateMutex(NULL, FALSE, NULL);
  if (!runtime->nonblocking_buffer_mutex) {
    set_error_step("create mutex");
    last_error_code = GetLastError();
    goto error;
  }

  HANDLE handles_to_inherit[] = {
    in_pipe_pseudo_console_side, out_pipe_pseudo_console_side
  };
  startup.StartupInfo.cb = sizeof(STARTUPINFOEXW);
  SIZE_T list_size = 0;
  InitializeProcThreadAttributeList(NULL, 2, 0, &list_size);
  startup.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(list_size);
  if (!startup.lpAttributeList
      || !InitializeProcThreadAttributeList(
        startup.lpAttributeList, 2, 0, &list_size)) {
    set_error_step("update proc attribute list");
    last_error_code = GetLastError();
    goto error;
  }
  attribute_list_initialized = TRUE;
  if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0,
        PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, runtime->hpcon, sizeof(HPCON),
        NULL, NULL)
      || !UpdateProcThreadAttribute(startup.lpAttributeList, 0,
        PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles_to_inherit,
        sizeof(handles_to_inherit), NULL, NULL)) {
    set_error_step("update proc attribute list");
    last_error_code = GetLastError();
    goto error;
  }

  commandline = windows_build_command_line(command, arguments);
  if (!commandline) {
    if (!last_error_code)
      last_error_code = ERROR_INVALID_PARAMETER;
    goto error;
  }
  if (cwd) {
    working_directory = windows_utf8_to_wide(cwd);
    if (!working_directory) {
      set_error_step("convert working directory to UTF-16");
      last_error_code = GetLastError();
      goto error;
    }
  }
  BOOL success = CreateProcessW(NULL, commandline, NULL, NULL, TRUE,
    EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
    environment && environment[0] ? (void*)environment[0] : NULL,
    working_directory, &startup.StartupInfo,
    &runtime->process_information);
  DWORD process_error = success ? ERROR_SUCCESS : GetLastError();
  if (in_pipe_pseudo_console_side) {
    CloseHandle(in_pipe_pseudo_console_side);
    in_pipe_pseudo_console_side = NULL;
  }
  if (out_pipe_pseudo_console_side) {
    CloseHandle(out_pipe_pseudo_console_side);
    out_pipe_pseudo_console_side = NULL;
  }
  free(working_directory);
  working_directory = NULL;
  free(commandline);
  commandline = NULL;
  if (attribute_list_initialized)
    DeleteProcThreadAttributeList(startup.lpAttributeList);
  free(startup.lpAttributeList);
  startup.lpAttributeList = NULL;
  attribute_list_initialized = FALSE;
  if (!success) {
    set_error_step("create process");
    last_error_code = process_error;
    goto error;
  }
  runtime->nonblocking_thread = CreateThread(NULL, 0,
    windows_nonblocking_thread_callback, runtime, 0, NULL);
  if (!runtime->nonblocking_thread) {
    set_error_step("create thread");
    last_error_code = GetLastError();
    goto error;
  }
  return runtime;

error:
  if (in_pipe_pseudo_console_side)
    CloseHandle(in_pipe_pseudo_console_side);
  if (out_pipe_pseudo_console_side)
    CloseHandle(out_pipe_pseudo_console_side);
  free(working_directory);
  free(commandline);
  if (attribute_list_initialized)
    DeleteProcThreadAttributeList(startup.lpAttributeList);
  free(startup.lpAttributeList);
  if (!last_error_code)
    last_error_code = FAILED(result) ? HRESULT_CODE(result) : GetLastError();
  terminal_runtime_close(runtime);
  free(runtime);
  return NULL;
#else
  struct termios termios = {0};
  termios.c_cc[VINTR] = 3;
  termios.c_cc[VSTART] = '\x13';
  termios.c_cc[VSTOP] = '\x11';
  termios.c_cc[VSUSP] = 26;
  termios.c_cc[VERASE] = '\x7F';
  termios.c_cc[VEOL] = 0;
  termios.c_cc[VEOF] = 4;
  termios.c_lflag |= ISIG | ECHO | ICANON | IEXTEN | ECHOE | ECHOK | ECHOCTL | ECHOKE;
  termios.c_cflag |= CS8 | CREAD;
  termios.c_iflag |= IUTF8 | ICRNL | IXON;
  termios.c_oflag |= OPOST | ONLCR | NL0 | CR0 | TAB0 | BS0 | VT0 | FF0;
  struct winsize size = {
    .ws_row = (unsigned short)rows,
    .ws_col = (unsigned short)columns,
    .ws_xpixel = 0,
    .ws_ypixel = 0
  };
  runtime->pid = forkpty(&runtime->master, NULL, &termios, &size);
  if (runtime->pid == -1 && set_error_step("forkpty")) {
    free(runtime);
    return NULL;
  }
  if (!runtime->pid) {
    if (cwd && chdir(cwd) != 0)
      _exit(127);
    if (term)
      setenv("TERM", term, 1);
    if (environment) {
      for (int i = 0; i < 256 && environment[i] && environment[i + 1]; i += 2)
        setenv(environment[i], environment[i + 1], 1);
    }
    const char* fallback_arguments[] = { command, NULL };
    execvp(command, (char* const*)(arguments ? arguments : fallback_arguments));
    _exit(127);
  }
  int flags = fcntl(runtime->master, F_GETFD, 0);
  fcntl(runtime->master, F_SETFL, flags | O_NONBLOCK);
  return runtime;
#endif
}

int terminal_runtime_write(terminal_runtime_t* runtime,
    const char* data, size_t length) {
  if (!runtime || runtime->closed || !data || !length)
    return 0;
#if _WIN32
  if (!runtime->topty)
    return 0;
  DWORD written = 0;
  DWORD requested = length > INT_MAX ? INT_MAX : (DWORD)length;
  if (!WriteFile(runtime->topty, data, requested, &written, NULL))
    return 0;
  return (int)written;
#else
  return (int)write(runtime->master, data, length);
#endif
}

void terminal_runtime_resize(terminal_runtime_t* runtime,
    int columns, int rows) {
  if (!runtime || runtime->closed)
    return;
#if _WIN32
  if (!runtime->hpcon || columns <= 0 || rows <= 0
      || columns > SHRT_MAX || rows > SHRT_MAX)
    return;
  COORD size = { columns, rows };
  ResizePseudoConsole(runtime->hpcon, size);
#else
  if (columns <= 0 || rows <= 0 || columns > USHRT_MAX || rows > USHRT_MAX)
    return;
  struct winsize size = {
    .ws_row = (unsigned short)rows,
    .ws_col = (unsigned short)columns,
    .ws_xpixel = 0,
    .ws_ypixel = 0
  };
  ioctl(runtime->master, TIOCSWINSZ, &size);
#endif
}

int terminal_runtime_poll(terminal_runtime_t* runtime,
    terminal_runtime_output_callback callback, void* user_data,
    int* total_shifts) {
  if (!runtime || runtime->closed)
    return 0;
  if (total_shifts)
    *total_shifts = 0;
#if _WIN32
  if (!runtime->nonblocking_buffer_mutex)
    return 0;
  char output[TERMINAL_RUNTIME_CHUNK_SIZE];
  int output_length = 0;
  WaitForSingleObject(runtime->nonblocking_buffer_mutex, INFINITE);
  if (runtime->nonblocking_buffer_length > 0) {
    output_length = runtime->nonblocking_buffer_length;
    memcpy(output, runtime->nonblocking_buffer, (size_t)output_length);
    runtime->nonblocking_buffer_length = 0;
  }
  ReleaseMutex(runtime->nonblocking_buffer_mutex);
  if (output_length > 0 && callback)
    callback(output, output_length, user_data);
  return output_length > 0;
#else
  char chunk[TERMINAL_RUNTIME_CHUNK_SIZE];
  int chunks_processed = 0;
  int available = 0;
  while (chunks_processed++ < TERMINAL_RUNTIME_MAX_CHUNKS) {
    ssize_t length = read(runtime->master, chunk, sizeof(chunk));
    if (length < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      return available;
    if (length <= 0)
      return available ? available : -1;
    if (callback)
      callback(chunk, (int)length, user_data);
    available = 1;
  }
  return available;
#endif
}

int terminal_runtime_exited(terminal_runtime_t* runtime,
    int* exit_code, int* signal) {
  if (!runtime || runtime->closed)
    return 0;
#if _WIN32
  DWORD code;
  if (!runtime->process_information.hProcess
      || !GetExitCodeProcess(runtime->process_information.hProcess, &code)
      || code == STILL_ACTIVE)
    return 0;
  if (exit_code) *exit_code = (int)code;
  if (signal) *signal = -1;
  return 1;
#else
  int status;
  if (waitpid(runtime->pid, &status, WNOHANG) <= 0)
    return 0;
  if (exit_code) *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  if (signal) *signal = WIFSIGNALED(status) ? WTERMSIG(status) : -1;
  runtime->pid = 0;
  return 1;
#endif
}

int terminal_runtime_close(terminal_runtime_t* runtime) {
  if (!runtime || runtime->closed)
    return 0;
  runtime->closed = 1;
#if _WIN32
  runtime->closing = 1;
  if (runtime->process_information.hProcess) {
    DWORD code = STILL_ACTIVE;
    if (GetExitCodeProcess(runtime->process_information.hProcess, &code)
        && code == STILL_ACTIVE) {
      TerminateProcess(runtime->process_information.hProcess, 1);
      WaitForSingleObject(runtime->process_information.hProcess, INFINITE);
    }
  }
  if (runtime->nonblocking_thread) {
    CancelSynchronousIo(runtime->nonblocking_thread);
    WaitForSingleObject(runtime->nonblocking_thread, INFINITE);
    CloseHandle(runtime->nonblocking_thread);
    runtime->nonblocking_thread = NULL;
  }
  if (runtime->frompty) {
    CloseHandle(runtime->frompty);
    runtime->frompty = NULL;
  }
  if (runtime->topty) {
    CloseHandle(runtime->topty);
    runtime->topty = NULL;
  }
  if (runtime->hpcon) {
    ClosePseudoConsole(runtime->hpcon);
    runtime->hpcon = NULL;
  }
  if (runtime->nonblocking_buffer_mutex) {
    CloseHandle(runtime->nonblocking_buffer_mutex);
    runtime->nonblocking_buffer_mutex = NULL;
  }
  if (runtime->process_information.hProcess) {
    CloseHandle(runtime->process_information.hProcess);
    runtime->process_information.hProcess = NULL;
  }
  if (runtime->process_information.hThread) {
    CloseHandle(runtime->process_information.hThread);
    runtime->process_information.hThread = NULL;
  }
#else
  if (runtime->master) {
    close(runtime->master);
    runtime->master = 0;
    if (runtime->pid)
      kill(runtime->pid, SIGHUP);
  }
  if (runtime->pid) {
    int status;
    if (waitpid(runtime->pid, &status, WNOHANG) == 0)
      return -1;
    runtime->pid = 0;
  }
#endif
  return 0;
}

void terminal_runtime_free(terminal_runtime_t* runtime) {
  if (!runtime)
    return;
  terminal_runtime_close(runtime);
#ifndef _WIN32
  if (runtime->pid)
    kill(runtime->pid, SIGKILL);
#endif
  free(runtime);
}
