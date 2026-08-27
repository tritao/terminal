#define PRAGTICAL_PLUGIN_ENTRYPOINT
#if _WIN32
  #include <windows.h>
#endif
#include <pragtical_plugin_api.h>
#include "../native/terminal_core.h"

#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TERMINAL_CHUNK_SIZE 4096

typedef struct terminal_binding {
  terminal_core_t* core;
} terminal_binding_t;

static terminal_binding_t* lua_toterminal(lua_State* L, int index) {
  lua_getfield(L, index, "__terminal");
  terminal_binding_t* terminal = (terminal_binding_t*)lua_touserdata(L, -1);
  lua_pop(L, 1);
  return terminal;
}

static int check_int(lua_State* L, int index) {
  lua_Integer value = luaL_checkinteger(L, index);
  if (value < INT_MIN || value > INT_MAX)
    luaL_error(L, "integer argument %d is out of range", index);
  return (int)value;
}

static char* duplicate_string(const char* source) {
  if (!source)
    return NULL;
  size_t length = strlen(source);
  if (length == SIZE_MAX)
    return NULL;
  char* result = malloc(length + 1);
  if (!result)
    return NULL;
  memcpy(result, source, length + 1);
  return result;
}

static void push_style(lua_State* L, uint64_t style, int* group) {
  char value[24];
  snprintf(value, sizeof(value), "%" PRIu64, style);
  lua_pushstring(L, value);
  lua_rawseti(L, -2, ++(*group));
}

typedef struct terminal_lines_context {
  lua_State* L;
  int result_index;
  int line_index;
  int current_row;
  int group;
  int line_count;
  int overflow;
  int last_text_index;
  int has_line;
} terminal_lines_context_t;

static void finish_line(terminal_lines_context_t* context) {
  lua_State* L = context->L;
  if (!context->has_line)
    return;

  if (!context->overflow && context->last_text_index > 0) {
    lua_rawgeti(L, context->line_index, context->last_text_index);
    lua_pushliteral(L, "\n");
    lua_concat(L, 2);
    lua_rawseti(L, context->line_index, context->last_text_index);
  }
  lua_rawseti(L, context->result_index, ++context->line_count);
  context->has_line = 0;
}

static void push_line_segment(int row, uint64_t style, const char* text,
    int length, int overflow, void* data) {
  terminal_lines_context_t* context = (terminal_lines_context_t*)data;
  lua_State* L = context->L;
  if (!context->has_line || row != context->current_row) {
    finish_line(context);
    lua_newtable(L);
    context->line_index = lua_gettop(L);
    context->current_row = row;
    context->group = 0;
    context->overflow = overflow;
    context->last_text_index = 0;
    context->has_line = 1;
  }

  push_style(L, style, &context->group);
  lua_pushlstring(L, text, (size_t)length);
  lua_rawseti(L, context->line_index, ++context->group);
  context->last_text_index = context->group;
  context->overflow = overflow;
}

static int f_terminal_lines(lua_State* L) {
  terminal_binding_t* terminal = lua_toterminal(L, 1);
  int rows;
  terminal_core_dimensions(terminal->core, NULL, &rows);
  int current, total;
  terminal_core_scrollback(terminal->core, -1, &current, &total);
  (void)total;

  int start = -current;
  if (lua_gettop(L) >= 2)
    start = check_int(L, 2);
  int end = start + rows;
  if (lua_gettop(L) >= 3) {
    int requested_end = check_int(L, 3);
    if (requested_end == INT_MAX)
      return luaL_error(L, "terminal line range is too large");
    end = requested_end + 1;
  }

  lua_newtable(L);
  terminal_lines_context_t context = {
    .L = L,
    .result_index = lua_gettop(L),
  };
  terminal_core_for_each_line(terminal->core, start, end - 1,
    push_line_segment, &context);
  finish_line(&context);
  return 1;
}

#if _WIN32
static const char* lua_toutf8(lua_State* L, LPCWSTR str) {
  int len = WideCharToMultiByte(CP_UTF8, 0, str, -1, NULL, 0, NULL, NULL);
  if (len > 0) {
    char* output = (char*)malloc(sizeof(char) * len);
    if (output) {
      len = WideCharToMultiByte(CP_UTF8, 0, str, -1, output, len, NULL, NULL);
      if (len) {
        lua_pushlstring(L, output, len - 1);
        free(output);
        return lua_tostring(L, -1);
      }
      free(output);
    }
  }
  return NULL;
}
#endif

static int f_terminal_new(lua_State* L) {
  int columns = check_int(L, 1);
  int rows = check_int(L, 2);
  int scrollback_limit = check_int(L, 3);
  const char* term = luaL_checkstring(L, 4);
  const char* command = luaL_checkstring(L, 5);
  char* arguments[256] = {0};
  char* environment[256] = {0};
  arguments[0] = (char*)command;

  if (lua_type(L, 6) == LUA_TTABLE) {
    for (int i = 0; i < 255; ++i) {
      lua_rawgeti(L, 6, i + 1);
      if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        break;
      }
      arguments[i + 1] = duplicate_string(luaL_checkstring(L, -1));
      lua_pop(L, 1);
    }
  }

#if _WIN32
  size_t environment_length;
  const char* environment_string = luaL_checklstring(L, 7, &environment_length);
  if (environment_length > INT_MAX)
    return luaL_error(L, "terminal environment is too large");
  LPWSTR utf16_environment = calloc(environment_length, sizeof(WCHAR));
  if (!utf16_environment)
    return luaL_error(L, "failed to allocate terminal environment");
  MultiByteToWideChar(CP_UTF8, 0, environment_string, (int)environment_length,
    utf16_environment, (int)environment_length);
  environment[0] = (char*)utf16_environment;
#else
  luaL_checktype(L, 7, LUA_TTABLE);
  lua_pushnil(L);
  int environment_index = 0;
  while (lua_next(L, 7) != 0 && environment_index < 255) {
    environment[environment_index++] = duplicate_string(lua_tostring(L, -2));
    environment[environment_index++] = duplicate_string(lua_tostring(L, -1));
    lua_pop(L, 1);
  }
#endif

  const char* cwd = lua_gettop(L) >= 9 ? luaL_optstring(L, 9, NULL) : NULL;
  terminal_core_t* core = terminal_core_new(columns, rows, scrollback_limit,
    term, command, (const char**)arguments, (const char**)environment, cwd);
  for (int i = 1; i < 256 && arguments[i]; ++i)
    free(arguments[i]);
  for (int i = 0; i < 256 && environment[i]; ++i)
    free(environment[i]);
  if (!core)
    return luaL_error(L, "error creating terminal: %s", terminal_core_last_error());

  terminal_binding_t* terminal = calloc(1, sizeof(*terminal));
  if (!terminal) {
    terminal_core_free(core);
    return luaL_error(L, "failed to allocate terminal binding");
  }
  terminal->core = core;
  terminal_core_set_debug(core, lua_toboolean(L, 8));

  lua_newtable(L);
  lua_pushlightuserdata(L, terminal);
  lua_setfield(L, -2, "__terminal");
  luaL_setmetatable(L, "libterminal");
  return 1;
}

#if _WIN32
static int f_terminal_getenv(lua_State* L) {
  LPWCH system_env = GetEnvironmentStringsW();
  LPWCH envp = system_env;
  lua_newtable(L);
  int table = lua_gettop(L);
  while (wcslen(envp) > 0) {
    const char* string = lua_toutf8(L, envp);
    if (string) {
      const char* equal = strstr(string, "=");
      lua_pushlstring(L, string, equal - string);
      lua_pushstring(L, equal + 1);
      lua_rawset(L, table);
      lua_pop(L, 1);
    }
    envp += wcslen(envp) + 1;
  }
  FreeEnvironmentStringsW(system_env);
  return 1;
}
#endif

static int f_terminal_gc(lua_State* L) {
  terminal_binding_t* terminal = lua_toterminal(L, 1);
  if (terminal) {
    terminal_core_free(terminal->core);
    free(terminal);
  }
  return 0;
}

static int f_terminal_close(lua_State* L) {
  terminal_binding_t* terminal = lua_toterminal(L, 1);
  lua_pushinteger(L, terminal_core_close(terminal->core));
  return 1;
}

typedef struct terminal_chunk_buffer {
  char* data;
  size_t length;
  size_t capacity;
  int failed;
} terminal_chunk_buffer_t;

static void chunk_update(char* data, int length, void* user_data) {
  terminal_chunk_buffer_t* chunks = (terminal_chunk_buffer_t*)user_data;
  if (chunks->failed || length <= 0)
    return;
  size_t required = chunks->length + (size_t)length;
  if (required > chunks->capacity) {
    size_t capacity = chunks->capacity ? chunks->capacity : TERMINAL_CHUNK_SIZE;
    while (capacity < required)
      capacity *= 2;
    char* buffer = realloc(chunks->data, capacity);
    if (!buffer) {
      chunks->failed = 1;
      return;
    }
    chunks->data = buffer;
    chunks->capacity = capacity;
  }
  memcpy(&chunks->data[chunks->length], data, (size_t)length);
  chunks->length = required;
}

static int f_terminal_update(lua_State* L) {
  int total_shifts = 0;
  terminal_chunk_buffer_t chunks = {0};
  int status = terminal_core_update(lua_toterminal(L, 1)->core,
    chunk_update, &chunks, &total_shifts);
  if (chunks.failed) {
    free(chunks.data);
    return luaL_error(L, "failed to buffer terminal output");
  }
  if (status != 0)
    lua_pushinteger(L, total_shifts);
  else
    lua_pushboolean(L, 0);
  lua_pushlstring(L, chunks.data ? chunks.data : "", chunks.length);
  free(chunks.data);
  return 2;
}

static int f_terminal_input(lua_State* L) {
  size_t length;
  const char* data = luaL_checklstring(L, 2, &length);
  lua_pushinteger(L, terminal_core_feed(lua_toterminal(L, 1)->core,
    data, (int)length));
  return 1;
}

static int f_terminal_checkpoint(lua_State* L) {
  terminal_core_t* core = lua_toterminal(L, 1)->core;
  size_t size = terminal_core_checkpoint_size(core);
  if (!size)
    return luaL_error(L, "failed to size terminal checkpoint");
  char* data = malloc(size);
  if (!data)
    return luaL_error(L, "failed to allocate terminal checkpoint");
  size_t written = 0;
  if (!terminal_core_checkpoint(core, data, size, &written)) {
    free(data);
    return luaL_error(L, "failed to serialize terminal checkpoint");
  }
  lua_pushlstring(L, data, written);
  free(data);
  return 1;
}

static int f_terminal_restore_checkpoint(lua_State* L) {
  size_t length;
  const char* data = luaL_checklstring(L, 2, &length);
  if (!terminal_core_restore_checkpoint(lua_toterminal(L, 1)->core,
      data, length)) {
    lua_pushboolean(L, 0);
    lua_pushliteral(L, "invalid terminal checkpoint");
    return 2;
  }
  lua_pushboolean(L, 1);
  return 1;
}

static int f_terminal_size(lua_State* L) {
  terminal_core_t* terminal = lua_toterminal(L, 1)->core;
  if (lua_gettop(L) > 1)
    terminal_core_resize(terminal, check_int(L, 2), check_int(L, 3));
  int columns, rows;
  terminal_core_dimensions(terminal, &columns, &rows);
  lua_pushinteger(L, columns);
  lua_pushinteger(L, rows);
  return 2;
}

static int f_terminal_exited(lua_State* L) {
  int exit_code, signal;
  if (terminal_core_exited(lua_toterminal(L, 1)->core, &exit_code, &signal)) {
    lua_pushinteger(L, exit_code);
#if _WIN32
    return 1;
#else
    lua_pushinteger(L, signal);
    return 2;
#endif
  }
  lua_pushboolean(L, 0);
  return 1;
}

static int f_terminal_cursor(lua_State* L) {
  int column, row, mode;
  terminal_core_cursor(lua_toterminal(L, 1)->core, &column, &row, &mode);
  lua_pushinteger(L, column);
  lua_pushinteger(L, row);
  switch (mode) {
    case TERMINAL_CORE_CURSOR_SOLID: lua_pushliteral(L, "solid"); break;
    case TERMINAL_CORE_CURSOR_HIDDEN: lua_pushliteral(L, "hidden"); break;
    default: lua_pushliteral(L, "blinking"); break;
  }
  return 3;
}

static int f_terminal_cursor_keys_mode(lua_State* L) {
  int mode;
  terminal_core_modes(lua_toterminal(L, 1)->core, &mode, NULL, NULL, NULL, NULL, NULL);
  lua_pushstring(L, mode == TERMINAL_CORE_KEYS_APPLICATION ? "application" : "normal");
  return 1;
}

static int f_terminal_keypad_keys_mode(lua_State* L) {
  int mode;
  terminal_core_modes(lua_toterminal(L, 1)->core, NULL, &mode, NULL, NULL, NULL, NULL);
  lua_pushstring(L, mode == TERMINAL_CORE_KEYS_APPLICATION ? "application" : "normal");
  return 1;
}

static int f_terminal_scrollback(lua_State* L) {
  int position = -1;
  if (lua_gettop(L) >= 2)
    position = check_int(L, 2);
  int current, total;
  terminal_core_scrollback(lua_toterminal(L, 1)->core, position, &current, &total);
  lua_pushinteger(L, current);
  lua_pushinteger(L, total);
  return 2;
}

static int f_terminal_focused(lua_State* L) {
  terminal_core_focus(lua_toterminal(L, 1)->core, lua_toboolean(L, 2));
  return 0;
}

static int f_terminal_paste_mode(lua_State* L) {
  int mode;
  terminal_core_modes(lua_toterminal(L, 1)->core, NULL, NULL, NULL, NULL, &mode, NULL);
  lua_pushstring(L, mode == TERMINAL_CORE_PASTE_BRACKETED ? "bracketed" : "normal");
  return 1;
}

static int f_terminal_name(lua_State* L) {
  const char* name = terminal_core_name(lua_toterminal(L, 1)->core);
  if (name)
    lua_pushstring(L, name);
  else
    lua_pushnil(L);
  return 1;
}

static int f_terminal_clear(lua_State* L) {
  terminal_core_clear(lua_toterminal(L, 1)->core);
  return 0;
}

static int f_terminal_clear_scrollback(lua_State* L) {
  terminal_core_clear_scrollback(lua_toterminal(L, 1)->core);
  return 0;
}

static int f_terminal_reset(lua_State* L) {
  terminal_core_reset(lua_toterminal(L, 1)->core);
  return 0;
}

static int f_terminal_mouse_tracking_mode(lua_State* L) {
  int mode;
  terminal_core_modes(lua_toterminal(L, 1)->core, NULL, NULL, &mode, NULL, NULL, NULL);
  switch (mode) {
    case TERMINAL_CORE_MOUSE_X10: lua_pushliteral(L, "x10"); break;
    case TERMINAL_CORE_MOUSE_NORMAL: lua_pushliteral(L, "normal"); break;
    case TERMINAL_CORE_MOUSE_BUTTON: lua_pushliteral(L, "button"); break;
    case TERMINAL_CORE_MOUSE_ANY: lua_pushliteral(L, "any"); break;
    default: lua_pushnil(L); break;
  }
  return 1;
}

static int f_terminal_mouse_encoding(lua_State* L) {
  int encoding;
  terminal_core_modes(lua_toterminal(L, 1)->core, NULL, NULL, NULL, &encoding, NULL, NULL);
  lua_pushstring(L, encoding == TERMINAL_CORE_MOUSE_ENCODING_SGR ? "sgr" : "default");
  return 1;
}

static int f_terminal_synchronized_output(lua_State* L) {
  lua_pushboolean(L, terminal_core_synchronized_output(
    lua_toterminal(L, 1)->core));
  return 1;
}

static int f_terminal_mouse(lua_State* L) {
  unsigned int cell_x = (unsigned int)check_int(L, 2);
  unsigned int cell_y = (unsigned int)check_int(L, 3);
  unsigned int button = (unsigned int)check_int(L, 4);
  unsigned int event = (unsigned int)check_int(L, 5);
  unsigned char modifiers = lua_gettop(L) >= 6
    ? (unsigned char)check_int(L, 6) : 0;
  lua_pushboolean(L, terminal_core_mouse(lua_toterminal(L, 1)->core,
    cell_x, cell_y, button, event, modifiers));
  return 1;
}

static const luaL_Reg terminal_api[] = {
  { "__gc",                f_terminal_gc                  },
  { "new",                 f_terminal_new                 },
  { "close",               f_terminal_close               },
  { "input",               f_terminal_input                },
  { "clear",               f_terminal_clear                },
  { "clear_scrollback",    f_terminal_clear_scrollback     },
  { "reset",               f_terminal_reset                },
  { "checkpoint",           f_terminal_checkpoint           },
  { "restore_checkpoint",   f_terminal_restore_checkpoint   },
  { "lines",               f_terminal_lines                },
  { "size",                f_terminal_size                 },
  { "update",              f_terminal_update               },
  { "exited",              f_terminal_exited               },
#if _WIN32
  { "getenv",              f_terminal_getenv                },
#endif
  { "cursor",              f_terminal_cursor                },
  { "focused",             f_terminal_focused               },
  { "mouse_tracking_mode", f_terminal_mouse_tracking_mode   },
  { "mouse_encoding",      f_terminal_mouse_encoding        },
  { "synchronized_output", f_terminal_synchronized_output    },
  { "mouse",              f_terminal_mouse                  },
  { "cursor_keys_mode",    f_terminal_cursor_keys_mode      },
  { "keypad_keys_mode",    f_terminal_keypad_keys_mode      },
  { "paste_mode",          f_terminal_paste_mode             },
  { "scrollback",          f_terminal_scrollback             },
  { "name",                f_terminal_name                   },
  { NULL,                   NULL                              }
};

#ifndef LIBTERMINAL_VERSION
#define LIBTERMINAL_VERSION "unknown"
#endif

#ifndef LIBTERMINAL_STANDALONE
int luaopen_pragtical_libterminal(lua_State* L, void* XL) {
  pragtical_plugin_init(XL);
#else
int luaopen_libterminal(lua_State* L) {
#endif
  luaL_newmetatable(L, "libterminal");
  luaL_setfuncs(L, terminal_api, 0);
  lua_pushliteral(L, LIBTERMINAL_VERSION);
  lua_setfield(L, -2, "version");
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");
  return 1;
}
