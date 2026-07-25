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
static DWORD windows_nonblocking_thread_callback(void* data) {
  terminal_runtime_t* runtime = (terminal_runtime_t*)data;
  char chunk[TERMINAL_RUNTIME_CHUNK_SIZE];
  while (1) {
    DWORD bytes_read;
    if (sizeof(chunk) - (size_t)runtime->nonblocking_buffer_length > 0
        || runtime->closing) {
      if (runtime->closing) {
        while (ReadFile(runtime->frompty, chunk, sizeof(chunk), &bytes_read, NULL)
            && bytes_read > 0) {
        }
        return 0;
      }
      if (!ReadFile(runtime->frompty, chunk,
          sizeof(chunk) - (size_t)runtime->nonblocking_buffer_length,
          &bytes_read, NULL))
        break;
      if (bytes_read > 0) {
        WaitForSingleObject(runtime->nonblocking_buffer_mutex, INFINITE);
        memcpy(&runtime->nonblocking_buffer[runtime->nonblocking_buffer_length],
          chunk, bytes_read);
        runtime->nonblocking_buffer_length += (int)bytes_read;
        ReleaseMutex(runtime->nonblocking_buffer_mutex);
      }
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
  HANDLE out_pipe_pseudo_console_side, in_pipe_pseudo_console_side;
  COORD size = { columns, rows };
  if ((!CreatePipe(&in_pipe_pseudo_console_side, &runtime->topty, &no_sec, 0)
      || !CreatePipe(&runtime->frompty, &out_pipe_pseudo_console_side, &no_sec, 0))
      && set_error_step("create pipes"))
    goto error;
  result = CreatePseudoConsole(size, in_pipe_pseudo_console_side,
    out_pipe_pseudo_console_side, 0, &runtime->hpcon);
  if (FAILED(result) && set_error_step("create pseudoconsole"))
    goto error;
  runtime->nonblocking_buffer_mutex = CreateMutex(NULL, FALSE, NULL);
  if (!runtime->nonblocking_buffer_mutex && set_error_step("create mutex"))
    goto error;

  HANDLE handles_to_inherit[] = {
    in_pipe_pseudo_console_side, out_pipe_pseudo_console_side
  };
  STARTUPINFOEXW startup = {0};
  startup.StartupInfo.cb = sizeof(STARTUPINFOEXW);
  startup.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
  size_t list_size;
  InitializeProcThreadAttributeList(NULL, 2, 0, &list_size);
  startup.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(list_size);
  BOOL success = InitializeProcThreadAttributeList(
      startup.lpAttributeList, 2, 0, (PSIZE_T)&list_size)
    && UpdateProcThreadAttribute(startup.lpAttributeList, 0,
      PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, runtime->hpcon, sizeof(HPCON), NULL, NULL)
    && UpdateProcThreadAttribute(startup.lpAttributeList, 0,
      PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles_to_inherit,
      sizeof(handles_to_inherit), NULL, NULL);
  if (!success && set_error_step("update proc attribute list"))
    goto error;

  int command_length = MultiByteToWideChar(CP_UTF8, 0, command, -1, NULL, 0);
  wchar_t* commandline = malloc(sizeof(wchar_t) * (size_t)(command_length + 1));
  if (!commandline && set_error_step("allocate command line"))
    goto error;
  MultiByteToWideChar(CP_UTF8, 0, command, -1, commandline, command_length);
  wchar_t* working_directory = NULL;
  if (cwd) {
    int cwd_length = MultiByteToWideChar(CP_UTF8, 0, cwd, -1, NULL, 0);
    working_directory = malloc(sizeof(wchar_t) * (size_t)cwd_length);
    if (working_directory)
      MultiByteToWideChar(CP_UTF8, 0, cwd, -1, working_directory, cwd_length);
  }
  success = CreateProcessW(NULL, commandline, NULL, NULL, TRUE,
    EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
    (void*)environment[0], working_directory, &startup.StartupInfo,
    &runtime->process_information);
  free(working_directory);
  free(commandline);
  DeleteProcThreadAttributeList(startup.lpAttributeList);
  free(startup.lpAttributeList);
  if (!success && set_error_step("create process"))
    goto error;
  runtime->nonblocking_thread = CreateThread(NULL, 0,
    windows_nonblocking_thread_callback, runtime, 0, NULL);
  if (!runtime->nonblocking_thread && set_error_step("create thread"))
    goto error;
  return runtime;

error:
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
    setenv("TERM", term, 1);
    for (int i = 0; i < 256 && environment[i]; i += 2)
      setenv(environment[i], environment[i + 1], 1);
    execvp(command, (char* const*)arguments);
    _exit(127);
  }
  int flags = fcntl(runtime->master, F_GETFD, 0);
  fcntl(runtime->master, F_SETFL, flags | O_NONBLOCK);
  return runtime;
#endif
}

int terminal_runtime_write(terminal_runtime_t* runtime,
    const char* data, size_t length) {
  if (!runtime || runtime->closed)
    return 0;
#if _WIN32
  DWORD written = 0;
  WriteFile(runtime->topty, data, (DWORD)length, &written, NULL);
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
  COORD size = { columns, rows };
  ResizePseudoConsole(runtime->hpcon, size);
#else
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
  int available = 0;
  WaitForSingleObject(runtime->nonblocking_buffer_mutex, INFINITE);
  if (runtime->nonblocking_buffer_length > 0) {
    if (callback)
      callback(runtime->nonblocking_buffer, runtime->nonblocking_buffer_length, user_data);
    runtime->nonblocking_buffer_length = 0;
    available = 1;
  }
  ReleaseMutex(runtime->nonblocking_buffer_mutex);
  return available;
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
  if (!GetExitCodeProcess(runtime->process_information.hProcess, &code)
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
  if (runtime->hpcon) {
    ClosePseudoConsole(runtime->hpcon);
    runtime->hpcon = NULL;
  }
  if (runtime->nonblocking_thread) {
    TerminateThread(runtime->nonblocking_thread, 0);
    CloseHandle(runtime->nonblocking_thread);
    runtime->nonblocking_thread = NULL;
  }
  if (runtime->topty) {
    CloseHandle(runtime->topty);
    runtime->topty = NULL;
  }
  if (runtime->frompty) {
    CloseHandle(runtime->frompty);
    runtime->frompty = NULL;
  }
  if (runtime->nonblocking_buffer_mutex) {
    CloseHandle(runtime->nonblocking_buffer_mutex);
    runtime->nonblocking_buffer_mutex = NULL;
  }
  if (runtime->process_information.hProcess) {
    TerminateProcess(runtime->process_information.hProcess, 1);
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
