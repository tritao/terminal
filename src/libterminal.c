#define PRAGTICAL_PLUGIN_ENTRYPOINT
#include <pragtical_plugin_api.h>
#include "../native/terminal_core.c"

static void push_style(lua_State* L, buffer_styling_t style, int* group) {
  uint64_t packed = (
    ((uint64_t)style.foreground.attributes << 56) |
    ((uint64_t)style.foreground.r << 48) |
    ((uint64_t)style.foreground.g << 40) |
    ((uint64_t)style.foreground.b << 32) |
    ((uint64_t)style.background.attributes << 24) |
    ((uint64_t)style.background.r << 16) |
    ((uint64_t)style.background.g << 8) |
    ((uint64_t)style.background.b << 0)
  );
  char hex_string[24];
  sprintf(hex_string, "%" PRIu64, packed);
  lua_pushstring(L, hex_string);
  lua_rawseti(L, -2, ++(*group));
}

static void output_line(lua_State* L, buffer_char_t* start, buffer_char_t* end, int overflows) {
  lua_newtable(L);
  int group = 0;
  int text_buffer_size = (int)(end - start) * 4 + 4;
  char* text_buffer = calloc(text_buffer_size, 1);
  if (!text_buffer)
    luaL_error(L, "failed to allocate terminal line buffer");

  buffer_char_t* line_start = start;
  buffer_char_t* last_nonzero = line_start - 1;
  for (buffer_char_t* cell = start; cell < end; ++cell) {
    if (cell->codepoint != 0)
      last_nonzero = cell;
  }
  if (last_nonzero < line_start) {
    push_style(L, start->styling, &group);
    lua_pushstring(L, overflows ? "" : "\n");
    lua_rawseti(L, -2, ++group);
    free(text_buffer);
    return;
  }

  buffer_styling_t style = start->styling;
  while (1) {
    int block_size = 0;
    while (start < end && start->styling.value == style.value && start <= last_nonzero) {
      block_size += codepoint_to_utf8(start->codepoint != 0 ? start->codepoint : ' ', &text_buffer[block_size]);
      ++start;
    }

    if (start > last_nonzero || start >= end || start->styling.value != style.value) {
      push_style(L, style, &group);
      lua_pushlstring(L, text_buffer, block_size);
      if (!overflows && start > last_nonzero) {
        lua_pushliteral(L, "\n");
        lua_concat(L, 2);
      }
      lua_rawseti(L, -2, ++group);
      if (start > last_nonzero)
        break;
      style = start->styling;
    }
  }
  free(text_buffer);
}


static terminal_t* lua_toterminal(lua_State* L, int index) {
  lua_getfield(L, index, "__terminal");
  terminal_t* terminal = (terminal_t*)lua_touserdata(L, -1);
  lua_pop(L, 1);
  return terminal;
}

static int f_terminal_lines(lua_State* L) {
  terminal_t* terminal = lua_toterminal(L, 1);
  int start = -terminal->scrollback_position;
  if (lua_gettop(L) >= 2)
    start = luaL_checkinteger(L, 2);
  int end = start + terminal->lines;
  if (lua_gettop(L) >= 3)
    end = luaL_checkinteger(L, 3) + 1;
  lua_newtable(L);

  int total_lines = 0;
  int remaining_lines = end - start;
  view_t* view = &terminal->views[terminal->current_view];
  if (terminal->current_view == VIEW_NORMAL_BUFFER && start < 0) {
    int top_offset = terminal->scrollback_target_top_offset;
    int offset = -start;
    backbuffer_page_t* current_backbuffer = terminal_find_scrollback_page(terminal, terminal->scrollback_target, &offset, &top_offset);
    int lines_into_buffer = top_offset - offset;
    while (current_backbuffer) {
      int* backbuffer_overflows = (int*)&current_backbuffer->buffer[LIBTERMINAL_BACKBUFFER_PAGE_LINES*current_backbuffer->columns];
      for (int y = lines_into_buffer; y < current_backbuffer->line; ++y) {
        output_line(L, &current_backbuffer->buffer[y * current_backbuffer->columns], &current_backbuffer->buffer[(y+1) * current_backbuffer->columns], backbuffer_overflows[y]);
        lua_rawseti(L, -2, ++total_lines);
        if (remaining_lines == 0)
          break;
      }
      current_backbuffer = current_backbuffer->next;
      lines_into_buffer = 0;
    }
    start = 0;
  }
  if (remaining_lines > 0) {
    remaining_lines = min(remaining_lines, terminal->lines);
    for (int y = 0; y < remaining_lines; ++y) {
      output_line(L, &view->buffer[(y + start) * terminal->columns], &view->buffer[(y + start + 1) * terminal->columns], view->overflows[y + start]);
      lua_rawseti(L, -2, ++total_lines);
    }
  }
  return 1;
}


#if _WIN32
static const char* lua_toutf8(lua_State* L, LPCWSTR str) {
  int len = WideCharToMultiByte(CP_UTF8, 0, str, -1, NULL, 0, NULL, NULL);
  if (len > 0) {
    char* output = (char *) malloc(sizeof(char) * len);
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
  int x = luaL_checkinteger(L, 1);
  int y = luaL_checkinteger(L, 2);
  int scrollback_limit = luaL_checkinteger(L, 3);
  const char* term_env = luaL_checkstring(L, 4);
  const char* path = luaL_checkstring(L, 5);
  char* arguments[256] = {0};
  char* environment[256] = {0};
  arguments[0] = (char*)path;
  arguments[1] = NULL;
  if (lua_type(L, 6) == LUA_TTABLE) {
    for (int i = 0; i < 255; ++i) {
      lua_rawgeti(L, 6, i+1);
      if (!lua_isnil(L, -1)) {
        const char* str = luaL_checkstring(L, -1);
        arguments[i+1] = strdup(str);
        lua_pop(L, 1);
      } else {
        lua_pop(L, 1);
        arguments[i+1] = NULL;
        break;
      }
    }
  }
  #if _WIN32
    size_t envlen;
    const char* env = luaL_checklstring(L, 7, &envlen);
    LPWSTR utf16_env = calloc(envlen, sizeof(WCHAR));
    MultiByteToWideChar(CP_UTF8, 0, env, envlen, utf16_env, envlen);
    environment[0] = (char*) utf16_env;
  #else
    luaL_checktype(L, 7, LUA_TTABLE);
    lua_pushnil(L);
    int i = 0;
    while (lua_next(L, 7) != 0 && i < 255) {
      environment[i] = strdup(lua_tostring(L, -2));
      environment[i+1] = strdup(lua_tostring(L, -1));
      i = i + 2;
      lua_pop(L, 1);
    }
  #endif
  int debug = lua_toboolean(L, 8);
  const char* cwd = lua_gettop(L) >= 9 ? luaL_optstring(L, 9, NULL) : NULL;
  terminal_t* terminal = terminal_core_new(x, y, scrollback_limit, term_env, path, (const char**)arguments, (const char**)environment, cwd);
  for (int i = 1; i < 256 && arguments[i]; ++i)
    free(arguments[i]);
  for (int i = 0; i < 256 && environment[i]; ++i)
    free(environment[i]);
  if (!terminal)
    return luaL_error(L, "error creating terminal: %s", terminal_get_last_error());
  terminal->debug = debug;
  lua_newtable(L);
  lua_pushlightuserdata(L, terminal);
  lua_setfield(L, -2, "__terminal");
  luaL_setmetatable(L, "libterminal");
  return 1;
}

#if _WIN32
static int f_terminal_getenv(lua_State* L) {
  LPWCH system_env = GetEnvironmentStringsW(), envp = system_env;
  lua_newtable(L);
  int table = lua_gettop(L);
  while (wcslen(envp) > 0) {
    const char* str = lua_toutf8(L, envp);
    if (str) {
      const char* equal = strstr(str, "=");
      lua_pushlstring(L, str, equal - str);
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
  terminal_core_free(lua_toterminal(L, 1));
  return 0;
}

static int f_terminal_close(lua_State* L) {
  lua_pushinteger(L, terminal_core_close(lua_toterminal(L, 1)));
  return 1;
}

typedef struct {
  char* data;
  size_t length;
  size_t capacity;
  int failed;
} terminal_chunk_buffer_t;

static void chunk_update(char* buf, int len, void* data) {
  terminal_chunk_buffer_t* chunks = (terminal_chunk_buffer_t*)data;
  if (chunks->failed || len <= 0)
    return;
  size_t required = chunks->length + (size_t)len;
  if (required > chunks->capacity) {
    size_t capacity = chunks->capacity ? chunks->capacity : LIBTERMINAL_CHUNK_SIZE;
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
  memcpy(&chunks->data[chunks->length], buf, (size_t)len);
  chunks->length = required;
}
static int f_terminal_update(lua_State* L){
  int status, total_shifts = 0;
  terminal_chunk_buffer_t chunks = {0};
  status = terminal_core_update(lua_toterminal(L, 1), chunk_update, &chunks, &total_shifts);
  if (chunks.failed) {
    free(chunks.data);
    return luaL_error(L, "failed to buffer terminal output");
  }
  if (status != 0)
    lua_pushinteger(L, total_shifts);
  else
    lua_pushboolean(L, 0);
  if (chunks.length > 0)
    lua_pushlstring(L, chunks.data, chunks.length);
  else
    lua_pushliteral(L, "");
  free(chunks.data);
  return 2;
}

static int f_terminal_input(lua_State* L) {
  size_t len;
  const char* str = luaL_checklstring(L, 2, &len);
  lua_pushinteger(L, terminal_core_feed(lua_toterminal(L, 1), str, (int)len));
  return 1;
}

static int f_terminal_size(lua_State* L) {
  terminal_t* terminal = lua_toterminal(L, 1);
  if (lua_gettop(L) > 1) {
    int x = luaL_checkinteger(L, 2), y = luaL_checkinteger(L, 3);
    terminal_core_resize(terminal, x, y);
  }
  int columns, lines;
  terminal_core_dimensions(terminal, &columns, &lines);
  lua_pushinteger(L, columns);
  lua_pushinteger(L, lines);
  return 2;
}


static int f_terminal_exited(lua_State* L) {
  terminal_t* terminal = lua_toterminal(L, 1);
  int exit_code, signal;
  if (terminal_core_exited(terminal, &exit_code, &signal)) {
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
  terminal_t* terminal = lua_toterminal(L, 1);
  lua_pushinteger(L, terminal->views[terminal->current_view].cursor_x);
  lua_pushinteger(L, terminal->views[terminal->current_view].cursor_y);
  switch (terminal->views[terminal->current_view].cursor_mode) {
    case CURSOR_SOLID: lua_pushliteral(L, "solid"); break;
    case CURSOR_HIDDEN: lua_pushliteral(L, "hidden"); break;
    case CURSOR_BLINKING: lua_pushliteral(L, "blinking"); break;
  }
  return 3;
}

static int f_terminal_cursor_keys_mode(lua_State* L) {
  terminal_t* terminal = lua_toterminal(L, 1);
  switch (terminal->views[terminal->current_view].cursor_keys_mode) {
    case KEYS_MODE_NORMAL: lua_pushliteral(L, "normal"); break;
    case KEYS_MODE_APPLICATION: lua_pushliteral(L, "application"); break;
  }
  return 1;
}

static int f_terminal_keypad_keys_mode(lua_State* L) {
  terminal_t* terminal = lua_toterminal(L, 1);
  switch (terminal->views[terminal->current_view].keypad_keys_mode) {
    case KEYS_MODE_NORMAL: lua_pushliteral(L, "normal"); break;
    case KEYS_MODE_APPLICATION: lua_pushliteral(L, "application"); break;
  }
  return 1;
}

static int f_terminal_scrollback(lua_State* L) {
  terminal_t* terminal = lua_toterminal(L, 1);
  if (terminal->current_view == VIEW_NORMAL_BUFFER) {
    if (lua_gettop(L) >= 2)
      terminal_scrollback(terminal, luaL_checkinteger(L, 2));
    lua_pushinteger(L, terminal->scrollback_position);
    lua_pushinteger(L, terminal->scrollback_total_lines);
  } else {
    lua_pushinteger(L, 0);
    lua_pushinteger(L, 0);
  }
  return 2;
}

static int f_terminal_focused(lua_State* L) {
  terminal_t* terminal = lua_toterminal(L, 1);
  if (terminal->reporting_focus)
    terminal_core_feed(terminal, lua_toboolean(L, 2) ? "\x1B[" : "\x1B[O", 3);
  return 0;
}

static int f_terminal_paste_mode(lua_State* L) {
  terminal_t* terminal = lua_toterminal(L, 1);
  lua_pushstring(L, terminal->paste_mode == PASTE_BRACKETED ? "bracketed" : "normal");
  return 1;
}

static int f_terminal_name(lua_State* L) {
  terminal_t* terminal = lua_toterminal(L, 1);
  if (terminal->name[0])
    lua_pushstring(L, terminal->name);
  else
    lua_pushnil(L);
  return 1;
}

static int f_terminal_clear(lua_State* L) {
  terminal_t* terminal = lua_toterminal(L, 1);
  terminal_clear_scrollback_buffer(terminal);
  view_t* view = &terminal->views[terminal->current_view];
  memset(view->buffer, 0, sizeof(buffer_char_t) * (terminal->columns * terminal->lines));
  view->cursor_x = 0;
  view->cursor_y = 0;
  return 0;
}

static int f_terminal_clear_scrollback(lua_State* L) {
  terminal_core_clear_scrollback(lua_toterminal(L, 1));
  return 0;
}

static int f_terminal_reset(lua_State* L) {
  terminal_core_reset(lua_toterminal(L, 1));
  return 0;
}

static int f_terminal_mouse_tracking_mode(lua_State* L) {
  terminal_t* terminal = lua_toterminal(L, 1);
  switch (terminal->mouse_tracking_mode) {
    case MOUSE_TRACKING_NONE: lua_pushnil(L); break;
    case MOUSE_TRACKING_X10: lua_pushliteral(L, "x10"); break;
    case MOUSE_TRACKING_NORMAL: lua_pushliteral(L, "normal"); break;
    case MOUSE_TRACKING_BUTTON: lua_pushliteral(L, "button"); break;
    case MOUSE_TRACKING_ANY: lua_pushliteral(L, "any"); break;
  }
  return 1;
}

static int f_terminal_mouse_encoding(lua_State* L) {
  terminal_t* terminal = lua_toterminal(L, 1);
  switch (terminal->mouse_encoding) {
    case MOUSE_ENCODING_DEFAULT: lua_pushliteral(L, "default"); break;
    case MOUSE_ENCODING_SGR: lua_pushliteral(L, "sgr"); break;
  }
  return 1;
}

static const luaL_Reg terminal_api[] = {
  { "__gc",                f_terminal_gc                     },
  { "new",                 f_terminal_new                    },
  { "close",               f_terminal_close                  },
  { "input",               f_terminal_input                  },
  { "clear",               f_terminal_clear                  },
  { "clear_scrollback",    f_terminal_clear_scrollback       },
  { "reset",               f_terminal_reset                  },
  { "lines",               f_terminal_lines                  },
  { "size",                f_terminal_size                   },
  { "update",              f_terminal_update                 },
  { "exited",              f_terminal_exited                 },
  #if _WIN32
  { "getenv",              f_terminal_getenv                 },
  #endif
  { "cursor",              f_terminal_cursor                 },
  { "focused",             f_terminal_focused                },
  { "mouse_tracking_mode", f_terminal_mouse_tracking_mode    },
  { "mouse_encoding",      f_terminal_mouse_encoding         },
  { "cursor_keys_mode",    f_terminal_cursor_keys_mode       },
  { "keypad_keys_mode",    f_terminal_keypad_keys_mode       },
  { "paste_mode",          f_terminal_paste_mode             },
  { "scrollback",          f_terminal_scrollback             },
  { "name",                f_terminal_name                   },
  { NULL,                  NULL                              }
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
