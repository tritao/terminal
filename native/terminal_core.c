#if _WIN32
  // https://devblogs.microsoft.com/commandline/windows-command-line-introducing-the-windows-pseudo-console-conpty/
  #if __MINGW32__ || __MINGW64__ // https://stackoverflow.com/questions/66419746/is-there-support-for-winpty-in-mingw-w64
    #define NTDDI_VERSION 0x0A000006 //NTDDI_WIN10_RS5
    #undef _WIN32_WINNT
    #define _WIN32_WINNT 0x0A00 // _WIN32_WINNT_WIN10
  #endif
  #include <windows.h>
  #include <wincon.h>
#else
  #include <unistd.h>
  #include <fcntl.h>
  #include <sys/ioctl.h>
  #include <sys/types.h>
  #include <sys/wait.h>
  #include <signal.h>
  #if __APPLE__
    #include <util.h>
  #else
    #include <pty.h>
  #endif
#endif
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include <inttypes.h>
#include "terminal_core.h"

#ifndef min
  static int min(int a, int b) { return a < b ? a : b; }
  static int max(int a, int b) { return a > b ? a : b; }
#endif

#define LIBTERMINAL_BACKBUFFER_PAGE_LINES 200
#define LIBTERMINAL_CHUNK_SIZE 4096
#define LIBTERMINAL_MAX_CHUNKS_PROCESSED 10
#define LIBTERMINAL_MAX_LINE_WIDTH 1024
#define LIBTERMINAL_NAME_MAX 256
#define LIBTERMINAL_DEFAULT_TAB_SIZE 8

typedef enum attributes_e {
  // Colors
  ATTRIBUTE_UNSET_COLOR = 0,
  ATTRIBUTE_INVERSE_COLOR = 1,
  ATTRIBUTE_INDEX_COLOR = 2,
  ATTRIBUTE_RGB_COLOR = 3,
  ATTRIBUTE_UNTARGETED_COLOR = 4,
  // Attributes
  ATTRIBUTE_BOLD = 8,
  ATTRIBUTE_ITALIC = 16,
  ATTRIBUTE_UNDERLINE = 32,
  ATTRIBUTE_STYLING_MASK = (32 | 16 | 8)
} attributes_e;

typedef struct color_t {
  union {
    struct {
      uint8_t attributes;
      union {
        struct {
          uint8_t r;
          uint8_t g;
          uint8_t b;
        };
        uint8_t index;
      };
    };
    uint32_t value;
  };
} color_t;
static color_t indexed_color(uint8_t index) { return (color_t) { .attributes = ATTRIBUTE_INDEX_COLOR, .index = index }; }
static color_t rgb_color(uint8_t r, uint8_t g, uint8_t b) { return (color_t) { .attributes = ATTRIBUTE_RGB_COLOR, .r = r, .g = g, .b = b }; }
static color_t UNSET_COLOR = { .attributes = ATTRIBUTE_UNSET_COLOR, .index = 0 };
static color_t INVERSE_COLOR = { .attributes = ATTRIBUTE_INVERSE_COLOR, .index = 0 };
static color_t UNTARGETED_COLOR = { .attributes = ATTRIBUTE_UNTARGETED_COLOR, .index = 0 };

#define LIBTERMINAL_NO_STYLING (buffer_styling_t) { UNSET_COLOR, UNSET_COLOR }

typedef struct buffer_styling_t {
  union {
    struct {
      color_t foreground;
      color_t background;
    };
    uint64_t value;
  };
} buffer_styling_t;

typedef struct buffer_char_t {
  buffer_styling_t styling;
  uint32_t codepoint;
} buffer_char_t;

typedef struct backbuffer_page_t {
  struct backbuffer_page_t* prev;
  struct backbuffer_page_t* next;
  int columns, lines, line;
  buffer_char_t buffer[];
} backbuffer_page_t;

typedef enum view_e {
  VIEW_NORMAL_BUFFER = 0,
  VIEW_ALTERNATE_BUFFER = 1,
  VIEW_MAX = 2
} view_e;

typedef enum cursor_mode_e {
  CURSOR_SOLID         = 0,
  CURSOR_HIDDEN        = 1,
  CURSOR_BLINKING      = 2,
} cursor_mode_e;

typedef enum paste_mode_e {
  PASTE_NORMAL,
  PASTE_BRACKETED
} paste_mode_e;

typedef enum keys_mode_e {
  KEYS_MODE_NORMAL,
  KEYS_MODE_APPLICATION
} keys_mode_e;

typedef enum mouse_tracking_mode_e {
  MOUSE_TRACKING_NONE,
  MOUSE_TRACKING_X10,
  MOUSE_TRACKING_NORMAL,
  MOUSE_TRACKING_BUTTON,
  MOUSE_TRACKING_ANY
} mouse_tracking_mode_e;

typedef enum mouse_encoding_e {
  MOUSE_ENCODING_DEFAULT,
  MOUSE_ENCODING_SGR
} mouse_encoding_e;

typedef enum charset_e {
  CHARSET_US,
  CHARSET_DEC,
  CHARSET_OTHER
} charset_e;

typedef struct view_t {
  buffer_char_t* buffer;
  int* overflows; // I don't like this, but as a result of how this is architected, this is necessary to ensure proper line outputs.
  int cursor_x, cursor_y;
  int cursor_styling_inversed;
  buffer_styling_t cursor_styling; // What characters are currently being emitted as.
  int saved_cursor_x, saved_cursor_y;
  int saved_cursor_styling_inversed;
  buffer_styling_t saved_cursor_styling;
  charset_e saved_charset;
  cursor_mode_e cursor_mode;
  keys_mode_e cursor_keys_mode;
  keys_mode_e keypad_keys_mode;
  color_t palette[256];                // Custom palette as per the ^][4;#;rgb:24/04/3C command. The fuck?
  charset_e charset;
  int last_graphical_character; // for CSI b
  int tab_size;
  // The index of where the scrolling region starts/ends.
  // If enabled, disables shuffling of text to the scrollback
  // buffer. When applied, shifts cursor to the top, and allows you to write
  // text wherever. However, if you hit newline, at the bottom of the scroll region
  // shifts only that region up.
  int scrolling_region_start, scrolling_region_end;
} view_t;

typedef enum mode_e {
  // Acts as a normal terminal, with a pty, and a shell.
  MODE_PTY,
  // Acts as a dummy; text is pumped in manually from lua.
  MODE_DUMMY
} mode_e;

typedef struct terminal_core {
  int debug;                                         // If true, dumps output to working directory in a file called `terminal.log`.
  backbuffer_page_t* scrollback_buffer_end;          // End of the linked list.
  backbuffer_page_t* scrollback_buffer_start;        // Beginning of linked list.
  backbuffer_page_t* scrollback_target;              // Target based on scrollback_position.
  int scrollback_target_top_offset;                  // The offset that the top of the scrollback_target page is from the start of the buffer.
  int scrollback_total_lines;                        // Cached total amount of lines we can scroll bcak.
  int scrollback_position;                           // Canonical amount of lines we've scrolled back.
  int scrollback_limit;                              // The amount of lines we'll hold in memory maximum.
  int columns, lines;
  view_e current_view;
  view_t views[VIEW_MAX];                            // Normally just two buffers, normal, and alternate.
  paste_mode_e paste_mode;
  mouse_tracking_mode_e mouse_tracking_mode;
  mouse_encoding_e mouse_encoding;
  mode_e mode;                                       // The mode the terminal is in.
  int closed;                                        // The runtime has been closed but the Lua object remains alive.
  int reporting_focus;                               // Enables/disbles reporting focus.
  char name[LIBTERMINAL_NAME_MAX];                   // Window name, set with OS command.
  char buffered_sequence[LIBTERMINAL_CHUNK_SIZE];
  #if _WIN32
    PROCESS_INFORMATION process_information;
    HPCON hpcon;
    HANDLE topty;
    HANDLE frompty;
    char nonblocking_buffer[LIBTERMINAL_CHUNK_SIZE];   // Oh my god, I hate windows so much.
    int nonblocking_buffer_length;
    HANDLE nonblocking_buffer_mutex;
    HANDLE nonblocking_thread;
    int closing;
  #else
    int master;                                        // FD for pty.
    pid_t pid;                                         // pid for shell.
  #endif
} terminal_t;


static int utf8_to_codepoint(const char *p, unsigned *dst) {
  const unsigned char *up = (unsigned char*)p;
  unsigned res, n;
  switch (*p & 0xf0) {
    case 0xf0 :  res = *up & 0x07;  n = 3;  break;
    case 0xe0 :  res = *up & 0x0f;  n = 2;  break;
    case 0xd0 :
    case 0xc0 :  res = *up & 0x1f;  n = 1;  break;
    default   :  res = *up;         n = 0;  break;
  }
  while (n--) {
    res = (res << 6) | (*(++up) & 0x3f);
  }
  *dst = res;
  return ((const char*)up + 1) - p;
}

static int codepoint_to_utf8(unsigned int codepoint, char* target) {
  if (codepoint < 128) {
    *(target++) = codepoint;
    return 1;
  } else if (codepoint < 2048) {
    *(target++) = 0xC0 | (codepoint >> 6);
    *(target++) = 0x80 | ((codepoint >> 0) & 0x3F);
    return 2;
  } else if (codepoint < 65536) {
    *(target++) = 0xE0 | (codepoint >> 12);
    *(target++) = 0x80 | ((codepoint >> 6) & 0x3F);
    *(target++) = 0x80 | ((codepoint >> 0) & 0x3F);
    return 3;
  }
  *(target++) = 0xF0 | (codepoint >> 18);
  *(target++) = 0x80 | ((codepoint >> 12) & 0x3F);
  *(target++) = 0x80 | ((codepoint >> 6) & 0x3F);
  *(target++) = 0x80 | ((codepoint >> 0) & 0x3F);
  return 4;
}

// Starts searching for the desired scrollback page based on offset, given the starting page of start; should pass NULL if you don't care.
// Target should contain the desired offset, top_offset should match the top offset of start, if start is not null.
static backbuffer_page_t* terminal_find_scrollback_page(terminal_t* terminal, backbuffer_page_t* start, int* offset, int* top_offset) {
  if (!terminal->scrollback_buffer_start || *offset <= 0) {
    *offset = 0;
    *top_offset = 0;
    return NULL;
  }
  while (*offset > *top_offset) {
    if (!start) {
      start = terminal->scrollback_buffer_start;
      *top_offset = start->line;
    } else {
      if (!start->prev) {
        *offset = *top_offset;
        return start;
      }
      start = start->prev;
      *top_offset += start->line;
    }
  }
  while (*offset < (*top_offset - start->line)) {
    if (!start->next) {
      *offset = 0;
      *top_offset = 0;
      return NULL;
    }
    *top_offset -= start->line;
    start = start->next;
  }
  return start;
}

static int terminal_scrollback(terminal_t* terminal, int target) {
  terminal->scrollback_target = terminal_find_scrollback_page(terminal, terminal->scrollback_target, &target, &terminal->scrollback_target_top_offset);
  terminal->scrollback_position = target;
  return terminal->scrollback_position;
}

static int terminal_output(terminal_t* terminal, const char* str, int len);
static int terminal_input(terminal_t* terminal, const char* str, int len) {
  if (terminal->closed)
    return 0;
  if (terminal->mode == MODE_PTY) {
    #ifdef _WIN32
      WriteFile(terminal->topty, str, len, NULL, NULL);
    #else
      write(terminal->master, str, len);
    #endif
    return 0;
  } else
    return terminal_output(terminal, str, len);
}

static void terminal_clear_scrollback_buffer(terminal_t* terminal) {
  backbuffer_page_t* scrollback_buffer = terminal->scrollback_buffer_start;
  while (scrollback_buffer) {
      backbuffer_page_t* prev = scrollback_buffer->prev;
      free(scrollback_buffer);
      scrollback_buffer = prev;
  }
  terminal->scrollback_buffer_start = NULL;
  terminal->scrollback_buffer_end = NULL;
  terminal->scrollback_target = NULL;
  terminal->scrollback_target_top_offset = 0;
  terminal->scrollback_total_lines = 0;
}

static void terminal_push_scrollback_line(terminal_t* terminal, const buffer_char_t* line, int overflow) {
  if (terminal->scrollback_total_lines++ > terminal->scrollback_limit) {
    backbuffer_page_t* page = terminal->scrollback_buffer_end;
    if (page->next)
      page->next->prev = NULL;
    terminal->scrollback_buffer_end = page->next;
    terminal->scrollback_total_lines -= page->line;
    free(page);
  }
  if (!terminal->scrollback_buffer_start || terminal->scrollback_buffer_start->columns != terminal->columns || terminal->scrollback_buffer_start->line >= terminal->scrollback_buffer_start->lines) {
    backbuffer_page_t* page = calloc(sizeof(backbuffer_page_t) + LIBTERMINAL_BACKBUFFER_PAGE_LINES*terminal->columns*sizeof(buffer_char_t) + sizeof(int)*LIBTERMINAL_BACKBUFFER_PAGE_LINES, 1);
    if (!terminal->scrollback_buffer_start)
      terminal->scrollback_buffer_end = page;
    backbuffer_page_t* prev = terminal->scrollback_buffer_start;
    page->prev = prev;
    if (prev)
      prev->next = page;
    terminal->scrollback_buffer_start = page;
    page->lines = LIBTERMINAL_BACKBUFFER_PAGE_LINES;
    page->columns = terminal->columns;
    page->line = 0;
  }
  memcpy(&terminal->scrollback_buffer_start->buffer[terminal->scrollback_buffer_start->line * terminal->columns], line, sizeof(buffer_char_t) * terminal->columns);
  int* backbuffer_overflows = (int*)&terminal->scrollback_buffer_start->buffer[LIBTERMINAL_BACKBUFFER_PAGE_LINES*terminal->scrollback_buffer_start->columns];
  backbuffer_overflows[terminal->scrollback_buffer_start->line] = overflow;
  terminal->scrollback_buffer_start->line++;
}

static int terminal_line_is_empty(terminal_t* terminal, const buffer_char_t* line, int overflow) {
  if (overflow)
    return 0;
  for (int i = 0; i < terminal->columns; ++i) {
    if (line[i].codepoint && line[i].codepoint != ' ')
      return 0;
  }
  return 1;
}

static void terminal_save_cursor(view_t* view) {
  view->saved_cursor_x = view->cursor_x;
  view->saved_cursor_y = view->cursor_y;
  view->saved_cursor_styling = view->cursor_styling;
  view->saved_cursor_styling_inversed = view->cursor_styling_inversed;
  view->saved_charset = view->charset;
}

static void terminal_restore_cursor(terminal_t* terminal, view_t* view) {
  view->cursor_x = min(max(view->saved_cursor_x, 0), terminal->columns - 1);
  view->cursor_y = min(max(view->saved_cursor_y, 0), terminal->lines - 1);
  view->cursor_styling = view->saved_cursor_styling;
  view->cursor_styling_inversed = view->saved_cursor_styling_inversed;
  view->charset = view->saved_charset;
}

static void terminal_blank_cells(buffer_char_t* cells, int count, buffer_styling_t styling) {
  for (int i = 0; i < count; ++i)
    cells[i] = (buffer_char_t){ styling, ' ' };
}

static void terminal_scroll_region_up(terminal_t* terminal, int start, int end, int count, int push_scrollback) {
  view_t* view = &terminal->views[terminal->current_view];
  start = min(max(start, 0), terminal->lines - 1);
  end = min(max(end, start + 1), terminal->lines);
  count = min(max(count, 1), end - start);

  if (push_scrollback && terminal->current_view == VIEW_NORMAL_BUFFER) {
    for (int y = start; y < start + count; ++y) {
      if (!terminal_line_is_empty(terminal, &view->buffer[terminal->columns * y], view->overflows[y]))
        terminal_push_scrollback_line(terminal, &view->buffer[terminal->columns * y], view->overflows[y]);
    }
  }

  if (end > start + count) {
    memmove(&view->buffer[terminal->columns * start], &view->buffer[terminal->columns * (start + count)], sizeof(buffer_char_t) * terminal->columns * (end - start - count));
    memmove(&view->overflows[start], &view->overflows[start + count], sizeof(int) * (end - start - count));
  }
  for (int y = end - count; y < end; ++y) {
    terminal_blank_cells(&view->buffer[terminal->columns * y], terminal->columns, view->cursor_styling);
    view->overflows[y] = 0;
  }
}

static void terminal_scroll_region_down(terminal_t* terminal, int start, int end, int count) {
  view_t* view = &terminal->views[terminal->current_view];
  start = min(max(start, 0), terminal->lines - 1);
  end = min(max(end, start + 1), terminal->lines);
  count = min(max(count, 1), end - start);

  if (end > start + count) {
    memmove(&view->buffer[terminal->columns * (start + count)], &view->buffer[terminal->columns * start], sizeof(buffer_char_t) * terminal->columns * (end - start - count));
    memmove(&view->overflows[start + count], &view->overflows[start], sizeof(int) * (end - start - count));
  }
  for (int y = start; y < start + count; ++y) {
    terminal_blank_cells(&view->buffer[terminal->columns * y], terminal->columns, view->cursor_styling);
    view->overflows[y] = 0;
  }
}

static void terminal_shift_buffer(terminal_t* terminal) {
  view_t* view = &terminal->views[terminal->current_view];

  if (view->scrolling_region_start != -1 && view->scrolling_region_end != -1) {
    // We perform this song and dance in case of Guldoman levels of resizing.
    terminal_scroll_region_up(terminal, view->scrolling_region_start, view->scrolling_region_end, 1, 1);
    return;
  }
  if (terminal->current_view == VIEW_NORMAL_BUFFER)
    terminal_push_scrollback_line(terminal, &view->buffer[0], view->overflows[0]);
  memmove(&view->buffer[0], &view->buffer[terminal->columns], sizeof(buffer_char_t) * terminal->columns * (terminal->lines - 1));
  memmove(&view->overflows[0], &view->overflows[1], sizeof(int) * (terminal->lines - 1));
  terminal_blank_cells(&view->buffer[terminal->columns * (terminal->lines - 1)], terminal->columns, view->cursor_styling);
  view->overflows[terminal->lines - 1] = 0;
}

static void terminal_switch_buffer(terminal_t* terminal, view_e view) {
  terminal->current_view = view;
  if (view == VIEW_ALTERNATE_BUFFER) {
    memset(terminal->views[VIEW_ALTERNATE_BUFFER].buffer, 0, sizeof(buffer_char_t) * terminal->columns * terminal->lines);
    memset(terminal->views[VIEW_ALTERNATE_BUFFER].overflows, 0, terminal->lines * sizeof(int));
    terminal->views[VIEW_ALTERNATE_BUFFER].cursor_x = 0;
    terminal->views[VIEW_ALTERNATE_BUFFER].cursor_y = 0;
    terminal->views[VIEW_ALTERNATE_BUFFER].cursor_styling = LIBTERMINAL_NO_STYLING;
    terminal->views[VIEW_ALTERNATE_BUFFER].cursor_styling_inversed = 0;
    terminal->views[VIEW_ALTERNATE_BUFFER].saved_cursor_x = 0;
    terminal->views[VIEW_ALTERNATE_BUFFER].saved_cursor_y = 0;
    terminal->views[VIEW_ALTERNATE_BUFFER].saved_cursor_styling = LIBTERMINAL_NO_STYLING;
    terminal->views[VIEW_ALTERNATE_BUFFER].saved_cursor_styling_inversed = 0;
    terminal->views[VIEW_ALTERNATE_BUFFER].saved_charset = CHARSET_US;
    terminal->views[VIEW_ALTERNATE_BUFFER].scrolling_region_end = -1;
    terminal->views[VIEW_ALTERNATE_BUFFER].scrolling_region_start = -1;
    for (int i = 0; i < 256; ++i)
      terminal->views[VIEW_ALTERNATE_BUFFER].palette[i] = indexed_color(i);
  }
}

static int parse_number(const char* seq, int def) {
  if (seq[0] >= '0' && seq[0] <= '9')
    return atoi(seq);
  return def;
}

typedef enum terminal_escape_type_e {
  ESCAPE_TYPE_NONE,
  ESCAPE_TYPE_OPEN,
  ESCAPE_TYPE_CSI,
  ESCAPE_TYPE_OS,
  ESCAPE_TYPE_FIXED_WIDTH,
  ESCAPE_TYPE_UNKNOWN
} terminal_escape_type_e;


static int terminal_escape_sequence(terminal_t* terminal, terminal_escape_type_e type, const char* seq) {
  #ifdef LIBTERMINAL_DEBUG_ESCAPE
  fprintf(stderr, "ESC");
  for (int i = 1; i < strlen(seq); ++i) {
    fprintf(stderr, "%c", seq[i]);
  }
  fprintf(stderr, "\n");
  #endif
  view_t* view = &terminal->views[terminal->current_view];
  int unhandled = 0;
  int end = (view->scrolling_region_end == -1 ? terminal->lines : view->scrolling_region_end);
  if (type == ESCAPE_TYPE_CSI) {
    int seq_end = strlen(seq) - 1;
    switch (seq[seq_end]) {
      case '@': {
        int length = min(max(parse_number(&seq[2], 1), 1), terminal->columns - view->cursor_x);
        memmove(&view->buffer[terminal->columns * view->cursor_y + view->cursor_x + length], &view->buffer[terminal->columns * view->cursor_y + view->cursor_x], sizeof(buffer_char_t) * max(terminal->columns - (view->cursor_x + length), 0));
        terminal_blank_cells(&view->buffer[terminal->columns * view->cursor_y + view->cursor_x], min(length, terminal->columns - view->cursor_x), view->cursor_styling);
      } break;
      case 'A': view->cursor_y = max(view->cursor_y - max(parse_number(&seq[2], 1), 1), 0);     break;
      case 'B': view->cursor_y = min(view->cursor_y + max(parse_number(&seq[2], 1), 1), terminal->lines - 1); break;
      case 'C': view->cursor_x = min(view->cursor_x + max(parse_number(&seq[2], 1), 1), terminal->columns - 1); break;
      case 'D': view->cursor_x = max(view->cursor_x - max(parse_number(&seq[2], 1), 1), 0); break;
      case 'E': view->cursor_y = min(view->cursor_y + max(parse_number(&seq[2], 1), 1), terminal->lines - 1); view->cursor_x = 0; break;
      case 'F': view->cursor_y = max(view->cursor_y - max(parse_number(&seq[2], 1), 1), 0); view->cursor_x = 0; break;
      case 'G': view->cursor_x = min(max(max(parse_number(&seq[2], 1), 1) - 1, 0), terminal->columns - 1); break;
      case 'f':
      case 'H': {
        int semicolon = -1;
        for (semicolon = 2; semicolon < seq_end && seq[semicolon] != ';'; ++semicolon);
        if (seq[semicolon] != ';') {
          view->cursor_x = 0;
          view->cursor_y = 0;
        } else {
          view->cursor_y = max(min(parse_number(&seq[2], 1) - 1, terminal->lines - 1), 0);
          view->cursor_x = max(min(parse_number(&seq[semicolon+1], 1) - 1, terminal->columns - 1), 0);
        }
      } break;
      case 'J': {
        switch (seq[2]) {
          case '1':
            for (int y = 0; y <= view->cursor_y; ++y) {
              int w = y == view->cursor_y ? (view->cursor_x+1) : terminal->columns;
              terminal_blank_cells(&view->buffer[terminal->columns * y], w, view->cursor_styling);
              if (w == terminal->columns)
                view->overflows[y] = 0;
            }
          break;
          case '3':
            terminal_clear_scrollback_buffer(terminal);
            // intentional fallthrough
          case '2':
            for (int y = 0; y < terminal->lines; ++y)
              terminal_blank_cells(&view->buffer[terminal->columns * y], terminal->columns, view->cursor_styling);
            memset(view->overflows, 0, sizeof(int) * terminal->lines);
            view->cursor_x = 0;
            view->cursor_y = 0;
          break;
          default:
            for (int y = view->cursor_y; y < terminal->lines; ++y) {
              int x = y == view->cursor_y ? view->cursor_x : 0;
              terminal_blank_cells(&view->buffer[terminal->columns * y + x], terminal->columns - x, view->cursor_styling);
              if (x == 0)
                view->overflows[y] = 0;
            }
          break;
        }
      } break;
      case 'K': {
        int s, e;
        switch (seq[2]) {
          case '1': s = 0; e = view->cursor_x + 1; break;
          case '2': s = 0; e = terminal->columns; break;
          default: s = view->cursor_x; e = terminal->columns; break;
        }
        for (int i = s; i < e; ++i)
          view->buffer[view->cursor_y * terminal->columns + i] = (buffer_char_t){ view->cursor_styling, ' ' };
      } break;
      case 'L': {
        int length = parse_number(&seq[2], 1);
        int start = view->scrolling_region_start == -1 ? 0 : view->scrolling_region_start;
        if (view->cursor_y >= start && view->cursor_y < end)
          terminal_scroll_region_down(terminal, view->cursor_y, end, length);
      } break;
      case 'M': {
        int length = parse_number(&seq[2], 1);
        int start = view->scrolling_region_start == -1 ? 0 : view->scrolling_region_start;
        if (view->cursor_y >= start && view->cursor_y < end)
          terminal_scroll_region_up(terminal, view->cursor_y, end, length, 0);
      } break;
      case 'P': {
        int length = parse_number(&seq[2], 1);
        for (int i = view->cursor_x; i < terminal->columns; ++i) {
          if (i + length < terminal->columns)
            view->buffer[view->cursor_y * terminal->columns + i] = view->buffer[view->cursor_y * terminal->columns + i + length];
          else
            view->buffer[view->cursor_y * terminal->columns + i] = (buffer_char_t){ view->cursor_styling, ' ' };
        }
      } break;
      case 'X': {
        int length = parse_number(&seq[2], 1);
        for (int i = view->cursor_x; i < view->cursor_x + length && i < terminal->columns; ++i)
          view->buffer[view->cursor_y * terminal->columns + i] = (buffer_char_t){ view->cursor_styling, ' ' };
      } break;
      case 'S': {
        terminal_scroll_region_up(terminal, view->scrolling_region_start == -1 ? 0 : view->scrolling_region_start, end, parse_number(&seq[2], 1), 0);
      } break;
      case 'T': {
        terminal_scroll_region_down(terminal, view->scrolling_region_start == -1 ? 0 : view->scrolling_region_start, end, parse_number(&seq[2], 1));
      } break;
      case 's': {
        terminal_save_cursor(view);
      } break;
      case 'u': {
        terminal_restore_cursor(terminal, view);
      } break;
      case 'b': {
        if (view->last_graphical_character) {
          int length = parse_number(&seq[2], 1);
          for (int i = view->cursor_x; i < min(view->cursor_x + length, terminal->columns); ++i)
            view->buffer[view->cursor_y * terminal->columns + i].codepoint = view->last_graphical_character;
        }
      } break;
      case 'c': {
        terminal_input(terminal, "\e[?1;2c", 7);
      } break;
      case 'd': view->cursor_y = min(max(max(parse_number(&seq[2], 1), 1) - 1, 0), terminal->lines - 1); break;
      case 'h': {
        if (seq[2] == '?') {
          const char* next = &seq[3];
          while (next) {
            switch (parse_number(next, 0)) {
              case 1: view->cursor_keys_mode = KEYS_MODE_APPLICATION; break;
              case 9: terminal->mouse_tracking_mode = MOUSE_TRACKING_X10; break;
              case 12: view->cursor_mode = CURSOR_BLINKING; break;
              case 25: view->cursor_mode = CURSOR_SOLID; break;
              case 1000: terminal->mouse_tracking_mode = MOUSE_TRACKING_NORMAL; break;
              case 1002: terminal->mouse_tracking_mode = MOUSE_TRACKING_BUTTON; break;
              case 1003: terminal->mouse_tracking_mode = MOUSE_TRACKING_ANY; break;
              case 1006: terminal->mouse_encoding = MOUSE_ENCODING_SGR; break;
              case 1004: terminal->reporting_focus = 1; break;
              case 1047: terminal_switch_buffer(terminal, VIEW_ALTERNATE_BUFFER); break;
              case 1049: terminal_switch_buffer(terminal, VIEW_ALTERNATE_BUFFER); break;
              case 2004: terminal->paste_mode = PASTE_BRACKETED; break;
              default: unhandled = 1; break;
            }
            if ((next = strstr(next, ";")))
              next++;
          }
        }
      } break;
      case 'l': {
        if (seq[2] == '?') {
          const char* next = &seq[3];
          while (next) {
            switch (parse_number(next, 0)) {
              case 1: view->cursor_keys_mode = KEYS_MODE_NORMAL; break;
              case 9:
              case 1000:
              case 1002:
              case 1003:
                terminal->mouse_tracking_mode = MOUSE_TRACKING_NONE;
              break;
              case 12: view->cursor_mode = CURSOR_SOLID; break;
              case 25: view->cursor_mode = CURSOR_HIDDEN; break;
              case 1006: terminal->mouse_encoding = MOUSE_ENCODING_DEFAULT; break;
              case 1004: terminal->reporting_focus = 0; break;
              case 1047: terminal_switch_buffer(terminal, VIEW_NORMAL_BUFFER); break;
              case 1049: terminal_switch_buffer(terminal, VIEW_NORMAL_BUFFER); break;
              case 2004: terminal->paste_mode = PASTE_NORMAL; break;
              default: unhandled = 1; break;
            }
            if (next = strstr(next, ";"))
              next++;
          }
        }
      } break;
      case 'm': {
        int offset = 2;
        enum DisplayState {
          DISPLAY_STATE_NONE,
          DISPLAY_STATE_COLOR_MODE,
          DISPLAY_STATE_COLOR_VALUE_IDX,
          DISPLAY_STATE_COLOR_VALUE_R,
          DISPLAY_STATE_COLOR_VALUE_G,
          DISPLAY_STATE_COLOR_VALUE_B
        };
        enum DisplayState state = DISPLAY_STATE_NONE;
        uint8_t r = 0,g = 0,b = 0;
        int foreground = 0;
        while (1) {
          color_t target_color = UNTARGETED_COLOR;
          int target_foreground = 0;
          switch (state) {
            case DISPLAY_STATE_NONE: {
              int display_number = parse_number(&seq[offset], 0);
              switch (display_number) {
                case 0  : view->cursor_styling = LIBTERMINAL_NO_STYLING; view->cursor_styling_inversed = 0; break;
                case 1  : view->cursor_styling.foreground.attributes |= ATTRIBUTE_BOLD; break;
                case 2  : break;
                case 3  : view->cursor_styling.foreground.attributes |= ATTRIBUTE_ITALIC; break;
                case 4  : view->cursor_styling.foreground.attributes |= ATTRIBUTE_UNDERLINE; break;
                case 22 : view->cursor_styling.foreground.attributes &= ~ATTRIBUTE_BOLD; break;
                case 23 : view->cursor_styling.foreground.attributes &= ~ATTRIBUTE_ITALIC; break;
                case 24 : view->cursor_styling.foreground.attributes &= ~ATTRIBUTE_UNDERLINE; break;
                case 27:
                case 7  : {
                  int is_inversed = display_number == 7;
                  if (is_inversed != view->cursor_styling_inversed) {
                    view->cursor_styling_inversed = is_inversed;
                    color_t background = view->cursor_styling.background;
                    if (view->cursor_styling.foreground.value == UNSET_COLOR.value)
                      view->cursor_styling.background = INVERSE_COLOR;
                    else if (view->cursor_styling.foreground.value == INVERSE_COLOR.value)
                      view->cursor_styling.background = UNSET_COLOR;
                    else
                      view->cursor_styling.background = view->cursor_styling.foreground;
                    if (background.value == UNSET_COLOR.value)
                      view->cursor_styling.foreground = INVERSE_COLOR;
                    else if (background.value == INVERSE_COLOR.value)
                      view->cursor_styling.foreground = UNSET_COLOR;
                    else
                      view->cursor_styling.foreground = view->cursor_styling.background;
                  }
                } break;
                case 30 : target_foreground = 1; target_color = view->palette[0]; break;
                case 31 : target_foreground = 1; target_color = view->palette[1]; break;
                case 32 : target_foreground = 1; target_color = view->palette[2]; break;
                case 33 : target_foreground = 1; target_color = view->palette[3]; break;
                case 34 : target_foreground = 1; target_color = view->palette[4]; break;
                case 35 : target_foreground = 1; target_color = view->palette[5]; break;
                case 36 : target_foreground = 1; target_color = view->palette[6]; break;
                case 37 : target_foreground = 1; target_color = view->palette[7]; break;
                case 38 : state = DISPLAY_STATE_COLOR_MODE; foreground = 1; break;
                case 39 : target_foreground = 1; target_color = UNSET_COLOR; break;
                case 40 : target_foreground = 0; target_color = view->palette[0]; break;
                case 41 : target_foreground = 0; target_color = view->palette[1]; break;
                case 42 : target_foreground = 0; target_color = view->palette[2]; break;
                case 43 : target_foreground = 0; target_color = view->palette[3]; break;
                case 44 : target_foreground = 0; target_color = view->palette[4]; break;
                case 45 : target_foreground = 0; target_color = view->palette[5]; break;
                case 46 : target_foreground = 0; target_color = view->palette[6]; break;
                case 47 : target_foreground = 0; target_color = view->palette[7]; break;
                case 48 : state = DISPLAY_STATE_COLOR_MODE; foreground = 0; break;
                case 49 : target_foreground = 0; target_color = UNSET_COLOR; break;
                case 90 : target_foreground = 1; target_color = view->palette[8]; break;
                case 91 : target_foreground = 1; target_color = view->palette[9]; break;
                case 92 : target_foreground = 1; target_color = view->palette[10]; break;
                case 93 : target_foreground = 1; target_color = view->palette[11]; break;
                case 94 : target_foreground = 1; target_color = view->palette[12]; break;
                case 95 : target_foreground = 1; target_color = view->palette[13]; break;
                case 96 : target_foreground = 1; target_color = view->palette[14]; break;
                case 97 : target_foreground = 1; target_color = view->palette[15]; break;
                case 100: target_foreground = 0; target_color = view->palette[8]; break;
                case 101: target_foreground = 0; target_color = view->palette[9]; break;
                case 102: target_foreground = 0; target_color = view->palette[10]; break;
                case 103: target_foreground = 0; target_color = view->palette[11]; break;
                case 104: target_foreground = 0; target_color = view->palette[12]; break;
                case 105: target_foreground = 0; target_color = view->palette[13]; break;
                case 106: target_foreground = 0; target_color = view->palette[14]; break;
                case 107: target_foreground = 0; target_color = view->palette[15]; break;
                default: unhandled = 1; break;
              }
            } break;
            case DISPLAY_STATE_COLOR_MODE: state = parse_number(&seq[offset], 0) != 5 ? DISPLAY_STATE_COLOR_VALUE_R : DISPLAY_STATE_COLOR_VALUE_IDX; break;
            case DISPLAY_STATE_COLOR_VALUE_IDX:
              target_foreground = foreground;
              int idx = (parse_number(&seq[offset], 0) & 0xFF);
              target_color = view->palette[idx];
              state = DISPLAY_STATE_NONE;
            break;
            case DISPLAY_STATE_COLOR_VALUE_R: r = (parse_number(&seq[offset], 0) & 0xFF); state = DISPLAY_STATE_COLOR_VALUE_G; break;
            case DISPLAY_STATE_COLOR_VALUE_G: g = (parse_number(&seq[offset], 0) & 0xFF); state = DISPLAY_STATE_COLOR_VALUE_B; break;
            case DISPLAY_STATE_COLOR_VALUE_B: {
              target_foreground = foreground;
              b = parse_number(&seq[offset], 0) & 0xFF;
              target_color = rgb_color(r, g, b);
              state = DISPLAY_STATE_NONE;
            } break;
          }
          if (target_color.value != UNTARGETED_COLOR.value) {
            if (view->cursor_styling_inversed)
              target_foreground = !target_foreground;
            if (target_foreground) {
              uint8_t attributes = view->cursor_styling.foreground.attributes;
              view->cursor_styling.foreground = target_color;
              view->cursor_styling.foreground.attributes |= (attributes & ATTRIBUTE_STYLING_MASK);
            } else
              view->cursor_styling.background = target_color;
          }
          const char* next = strchr(&seq[offset], ';');
          if (!next)
            break;
          offset = (next - seq) + 1;
        }
        return 0;
      } break;
      case 'n': {
        if (parse_number(&seq[2], 0) == 6) {
          // CPR responses are one-based and must end with R; fish waits for the complete sequence.
          char buffer[32];
          int length = snprintf(buffer, sizeof(buffer), "\x1B[%d;%dR", view->cursor_y + 1, view->cursor_x + 1);
          terminal_input(terminal, buffer, length);
        } else
          unhandled = 1;
      } break;
      case 'r': {
        int semicolon = -1;
        for (semicolon = 2; semicolon < seq_end && seq[semicolon] != ';'; ++semicolon);
        view->cursor_x = 0;
        view->cursor_y = 0;
        if (seq[semicolon] == ';') {
          view->scrolling_region_start = min(max(parse_number(&seq[2], 1) - 1, 0), terminal->lines - 1);
          view->scrolling_region_end = min(max(parse_number(&seq[semicolon+1], 1), 0), terminal->lines);
        }
      } break;
      default: unhandled = 1; break;
    }
  } else if (type == ESCAPE_TYPE_OS) {
    switch (seq[2]) {
      case '0':
        if (strlen(seq) >= 5 && seq[3] == ';')
          strncpy(terminal->name, &seq[4], min(sizeof(terminal->name) - 1, strlen(seq) - 4));
      break;
      case '4': {
        int idx, r,g,b;
        if (sscanf(&seq[3], ";%d;rgb:%x/%x/%x", &idx, &r, &g, &b) == 4) {
          view->palette[idx] = rgb_color(r, g, b);
        } else
          unhandled = 1;
      } break;
      default: unhandled = 1; break;
    }
  } else if (type == ESCAPE_TYPE_FIXED_WIDTH) {
    switch (seq[1]) {
      case '#': { // Put in, to satisfy vttest.
        switch (seq[2]) {
          case '8':
            for (int y = 0; y < terminal->lines; ++y) {
              for (int x = 0; x < terminal->columns; ++x)
                view->buffer[y * terminal->columns + x] = (buffer_char_t){ view->cursor_styling, 'E' };
            }
          break;
          default: unhandled = 1; break;
        }
      } break;
      case 'D':
      case 'E': {
        int end = (view->scrolling_region_end == -1 ? terminal->lines : min(view->scrolling_region_end, terminal->lines));
        if (view->cursor_y == end - 1)
          terminal_shift_buffer(terminal);
        else
          view->cursor_y = min(view->cursor_y + 1, terminal->lines - 1);
        if (seq[1] == 'E')
          view->cursor_x = 0;
      } break;
      case '(':
        switch (seq[2]) {
          case '0': view->charset = CHARSET_DEC; break;
          case 'B': view->charset = CHARSET_US; break;
          default: view->charset = CHARSET_OTHER; break;
        }
      break;
      case '=': view->keypad_keys_mode = KEYS_MODE_APPLICATION; break;
      case '>': view->keypad_keys_mode = KEYS_MODE_NORMAL; break;
      case '7': terminal_save_cursor(view); break;
      case '8': terminal_restore_cursor(terminal, view); break;
      case 'M':
        if (view->scrolling_region_start != -1 && view->scrolling_region_end != -1) {
          int start = min(view->scrolling_region_start, terminal->lines - 1);
          int end = min(view->scrolling_region_end, terminal->lines);
          if (view->cursor_y == start) {
            terminal_scroll_region_down(terminal, start, end, 1);
          } else if (view->cursor_y > start) {
            --view->cursor_y;
          }
        } else if (view->cursor_y == 0) {
          terminal_scroll_region_down(terminal, 0, terminal->lines, 1);
        } else {
          --view->cursor_y;
        }
      break;
      default: unhandled = 1; break;
    }
  }

  if (unhandled) {
    #ifdef LIBTERMINAL_DEBUG_ESCAPE
      fprintf(stderr, "UNKNOWN ESCAPE SEQUENCE\n");
    #endif
    return -1;
  }
  return 0;
}


static terminal_escape_type_e get_terminal_escape_type(char a, int* fixed_width) {
  switch (a) {
    case '[': return ESCAPE_TYPE_CSI;
    case ']': return ESCAPE_TYPE_OS;
    case 'D':
    case 'E':
    case 'H':
    case 'M':
    case 'Z':
    case 'F':
    case '>':
    case '=':
    case '7':
    case '8':
    case 'c':
    case 'l':
    case 'm':
    case 'n':
    case 'o':
    case '|':
    case '}':
    case '~':
      *fixed_width = 2;
      return ESCAPE_TYPE_FIXED_WIDTH;
    case ' ':
    case '#':
    case '%':
    case '(':
    case ')':
    case '*':
    case '+':
      *fixed_width = 3;
      return ESCAPE_TYPE_FIXED_WIDTH;
  }
  return ESCAPE_TYPE_UNKNOWN;
}

static terminal_escape_type_e parse_partial_sequence(const char* seq, int len, int* fixed_width) {
  if (len == 0)
    return ESCAPE_TYPE_NONE;
  if (len == 1)
    return ESCAPE_TYPE_OPEN;
  return get_terminal_escape_type(seq[1], fixed_width);
}

static int translate_charset(charset_e charset, int codepoint) {
  if (charset == CHARSET_DEC) {
    switch (codepoint) {
      case 0x5F: codepoint = ' '; break;
      case 0x60: codepoint = 0x25C6; break;
      case 0x61: codepoint = 0x2592; break;
      case 0x62: codepoint = '\t'; break;
      case 0x63: codepoint = '\f'; break;
      case 0x64: codepoint = '\r'; break;
      case 0x65: codepoint = '\n'; break;
      case 0x66: codepoint = 0xB0; break;
      case 0x67: codepoint = 0xB1; break;
      case 0x68: codepoint = '\n'; break;
      case 0x69: codepoint = '\v'; break;
      case 0x6A: codepoint = 0x2518; break;
      case 0x6B: codepoint = 0x2510; break;
      case 0x6C: codepoint = 0x250C; break;
      case 0x6D: codepoint = 0x2514; break;
      case 0x6E: codepoint = 0x253C; break;
      case 0x70: codepoint = 0x23BB; break;
      case 0x71: codepoint = 0x2500; break;
      case 0x72: codepoint = 0x23BC; break;
      case 0x73: codepoint = 0x23BD; break;
      case 0x74: codepoint = 0x251C; break;
      case 0x75: codepoint = 0x2524; break;
      case 0x76: codepoint = 0x2534; break;
      case 0x77: codepoint = 0x252C; break;
      case 0x78: codepoint = 0x2502; break;
      case 0x79: codepoint = 0x2264; break;
      case 0x7A: codepoint = 0x2265; break;
      case 0x7B: codepoint = 0x03C0; break;
      case 0x7C: codepoint = 0x2260; break;
      case 0x7D: codepoint = 0x00A3; break;
      case 0x7E: codepoint = 0x00B7; break;
    }
  }
  return codepoint;
}

static int terminal_output(terminal_t* terminal, const char* str, int len) {
  if (terminal->debug)  {
    FILE* file = fopen("terminal.log", "ab");
    if (file) {
      fwrite(str, sizeof(char), len, file);
      fclose(file);
    }
  }
  unsigned int codepoint;
  int total_shifts = 0;
  int offset = 0;
  int buffered_sequence_index = strlen(terminal->buffered_sequence);
  view_t* view = &terminal->views[terminal->current_view];
  int fixed_width = -1;
  terminal_escape_type_e escape_type = parse_partial_sequence(terminal->buffered_sequence, buffered_sequence_index, &fixed_width);
  while (offset < len) {
    if (escape_type != ESCAPE_TYPE_NONE) {
      if (buffered_sequence_index >= (int)sizeof(terminal->buffered_sequence) - 2) {
        buffered_sequence_index = 0;
        terminal->buffered_sequence[0] = 0;
        escape_type = ESCAPE_TYPE_NONE;
        continue;
      }
      terminal->buffered_sequence[buffered_sequence_index++] = str[offset];
      escape_type = parse_partial_sequence(terminal->buffered_sequence, buffered_sequence_index, &fixed_width);
      if (
        (escape_type == ESCAPE_TYPE_CSI && buffered_sequence_index > 2 && str[offset] >= 0x40 && str[offset] <= 0x7E) ||
        (escape_type == ESCAPE_TYPE_OS && ((offset < len - 1 && (str[offset+1] == '\a') || (offset < len - 2 && str[offset+1] == 0x1B && str[offset+2] == 0x5C)))) ||
        (escape_type == ESCAPE_TYPE_UNKNOWN && str[offset] == 0x1B) ||
        (escape_type == ESCAPE_TYPE_FIXED_WIDTH && buffered_sequence_index == fixed_width || str[offset] == 0x1B)
      ) {
        terminal->buffered_sequence[buffered_sequence_index++] = 0;
        terminal_escape_sequence(terminal, escape_type, terminal->buffered_sequence);
        view->last_graphical_character = 0;
        view = &terminal->views[terminal->current_view];
        buffered_sequence_index = 0;
        terminal->buffered_sequence[0] = 0;
        if (escape_type == ESCAPE_TYPE_OS) {
          escape_type = ESCAPE_TYPE_NONE;
          offset += str[offset+1] == 0x1B ? 2 : 1;
        } else if (str[offset] == 0x1B && (escape_type == ESCAPE_TYPE_UNKNOWN || escape_type == ESCAPE_TYPE_FIXED_WIDTH)) {
          escape_type = ESCAPE_TYPE_OPEN;
          terminal->buffered_sequence[buffered_sequence_index++] = str[offset];
        } else {
          escape_type = ESCAPE_TYPE_NONE;
        }
      }
      ++offset;
    } else {
      int end = (view->scrolling_region_end == -1 ? terminal->lines : view->scrolling_region_end);
      offset += utf8_to_codepoint(&str[offset], &codepoint);
      if (codepoint != '\e')
        view->last_graphical_character = 0;
      switch (codepoint) {
        case 0x00:
        case 0x01:
        case 0x02:
        case 0x03:
        case 0x04:
        case 0x05:
        case 0x06:
        case 0x07:
        break;
        case '\b': {
          if (view->cursor_x)
            --view->cursor_x;
        } break;
        case '\t': {
          view->cursor_x = (view->cursor_x + view->tab_size) - ((view->cursor_x + view->tab_size) % view->tab_size);
        } break;
        case '\n': {
          // So that we can copy text blocks properly.
          if (view->cursor_y < (end - 1))
            ++view->cursor_y;
          else {
            terminal_shift_buffer(terminal);
            ++total_shifts;
          }
        } break;
        case 0x0B:
        case 0x0C:
        break;
        case '\r': {
          view->cursor_x = 0;
        } break;
        case 0x0E:
        case 0x0F:
        case 0x11:
        case 0x12:
        case 0x13:
        case 0x14:
        case 0x15:
        case 0x16:
        case 0x17:
        case 0x18:
        case 0x19:
        case 0x1A:
          break;
        case 0x1B: { // escape
          terminal->buffered_sequence[0] = 0x1B;
          escape_type = ESCAPE_TYPE_OPEN;
          buffered_sequence_index = 1;
        } break;
        case 0x1C:
        case 0x1D:
        case 0x1E:
        case 0x1F:
          break;
        default:
          if (view->cursor_x >= terminal->columns) {
            view->overflows[view->cursor_y] = 1;
            view->cursor_x = 0;
            if (view->cursor_y < (end - 1))
              ++view->cursor_y;
            else {
              terminal_shift_buffer(terminal);
              ++total_shifts;
            }
          }
          codepoint = translate_charset(view->charset, codepoint);
          view->buffer[view->cursor_y * terminal->columns + view->cursor_x] = (buffer_char_t){ view->cursor_styling, codepoint };
          view->last_graphical_character = codepoint;
          view->cursor_x++;
        break;
      }
    }
  }
  terminal->buffered_sequence[buffered_sequence_index] = 0;
  return total_shifts;
}

#ifdef _WIN32
  static DWORD windows_nonblocking_thread_callback(void* data) {
    terminal_t* terminal = (terminal_t*)data;
    char chunk_buffer[LIBTERMINAL_CHUNK_SIZE];
    while (1) {
      DWORD bytes_read;
      if (sizeof(chunk_buffer) - terminal->nonblocking_buffer_length > 0 || terminal->closing) {
        if (terminal->closing) {
          while (1) {
            if (!ReadFile(terminal->frompty, chunk_buffer, sizeof(chunk_buffer), &bytes_read, NULL) || bytes_read == 0)
              break;
          }
          return 0;
        }
        if (!ReadFile(terminal->frompty, chunk_buffer, sizeof(chunk_buffer) - terminal->nonblocking_buffer_length, &bytes_read, NULL))
          break;
        if (bytes_read > 0) {
          WaitForSingleObject(terminal->nonblocking_buffer_mutex, INFINITE);
          memcpy(&terminal->nonblocking_buffer[terminal->nonblocking_buffer_length], chunk_buffer, bytes_read);
          terminal->nonblocking_buffer_length += bytes_read;
          ReleaseMutex(terminal->nonblocking_buffer_mutex);
        }
      }
      Sleep(1);
    }
    return 0;
  }
#endif

static int terminal_update(terminal_t* terminal, void (*callback)(char*, int, void*), void* data, int* total_shifts) {
  if (terminal->closed || terminal->mode == MODE_DUMMY)
    return 0;
  char chunk[LIBTERMINAL_CHUNK_SIZE];
  int len, at_least_one = 0;
  #ifdef _WIN32
    WaitForSingleObject(terminal->nonblocking_buffer_mutex, INFINITE);
    if (terminal->nonblocking_buffer_length > 0) {
      *total_shifts += terminal_output(terminal, terminal->nonblocking_buffer, terminal->nonblocking_buffer_length);
      if (callback)
        callback(terminal->nonblocking_buffer, terminal->nonblocking_buffer_length, data);
      at_least_one = 1;
    }
    terminal->nonblocking_buffer_length = 0;
    ReleaseMutex(terminal->nonblocking_buffer_mutex);
    return at_least_one;
  #else
    int chunks_processed = 0;
    do {
      len = read(terminal->master, chunk, sizeof(chunk));
      if (len == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return at_least_one;
      if (len <= 0)
        return at_least_one ? at_least_one : -1;
      *total_shifts += terminal_output(terminal, chunk, len);
      if (callback)
        callback(chunk, len, data);
      at_least_one = 1;
    } while (len > 0 && chunks_processed++ < LIBTERMINAL_MAX_CHUNKS_PROCESSED);
  #endif
  return -1;
}

static int terminal_close(terminal_t* terminal) {
  if (terminal->closed)
    return 0;
  terminal->closed = 1;
  terminal_clear_scrollback_buffer(terminal);
  for (int i = 0; i < VIEW_MAX; ++i) {
    if (terminal->views[i].buffer) {
      free(terminal->views[i].buffer);
      free(terminal->views[i].overflows);
    }
    terminal->views[i].buffer = NULL;
    terminal->views[i].overflows = NULL;
  }
  if (terminal->mode == MODE_PTY) {
    #if _WIN32
      // This has to be first, because if we don't drain the buffer in our nonblocking_thread,
      // this call can hang. (SIGH).
      terminal->closing = 1;
      if (terminal->hpcon) {
        ClosePseudoConsole(terminal->hpcon);
        terminal->hpcon = NULL;
      }
      if (terminal->nonblocking_thread) {
        TerminateThread(terminal->nonblocking_thread, 0);
        terminal->nonblocking_thread = NULL;
      }
      if (terminal->topty) {
        CloseHandle(terminal->topty);
        terminal->topty = NULL;
      }
      if (terminal->frompty) {
        CloseHandle(terminal->frompty);
        terminal->frompty = NULL;
      }
      if (terminal->nonblocking_buffer_mutex) {
        CloseHandle(terminal->nonblocking_buffer_mutex);
        terminal->nonblocking_buffer_mutex = NULL;
      }
      if (terminal->process_information.hProcess) {
        TerminateProcess(terminal->process_information.hProcess, 1);
        terminal->process_information.hProcess = NULL;
      }
    #else
      if (terminal->pid) {
        if (terminal->master) {
          close(terminal->master);
          terminal->master = 0;
          kill(terminal->pid, SIGHUP);
        }
        int status;
        if (waitpid(terminal->pid, &status, WNOHANG))
          terminal->pid = 0;
        else
          return -1;
      }
    #endif
  }
  return 0;
}

static void terminal_free(terminal_t* terminal) {
  terminal_close(terminal);
  #ifdef _WIN32
  #else
    if (terminal->pid)
      kill(terminal->pid, SIGKILL);
  #endif
  free(terminal);
}

static void terminal_resize(terminal_t* terminal, int columns, int lines) {
  if (terminal->columns == columns && terminal->lines == lines)
    return;
  if (terminal->mode == MODE_PTY) {
    #ifdef _WIN32
      COORD size = { columns, lines };
      ResizePseudoConsole(terminal->hpcon, size);
    #else
      struct winsize size = { .ws_row = lines, .ws_col = columns, .ws_xpixel = 0, .ws_ypixel = 0 };
      ioctl(terminal->master, TIOCSWINSZ, &size);
    #endif
  }
  for (int i = 0; i < VIEW_MAX; ++i) {
    buffer_char_t* buffer = malloc(sizeof(buffer_char_t) * columns * lines);
    int* overflows = calloc(lines * sizeof(int), 1);
    memset(buffer, 0, sizeof(buffer_char_t) * columns * lines);
    if (terminal->views[i].buffer) {
      if (lines < terminal->lines && i == VIEW_NORMAL_BUFFER) {
        for (int j = 0; j < max(0, (terminal->views[i].cursor_y+1) - lines); ++j)
          terminal_shift_buffer(terminal);
      }
      int max_lines = min(terminal->lines, lines);
      for (int y = 0; y < max_lines; ++y)
        memcpy(&buffer[y*columns], &terminal->views[i].buffer[y*terminal->columns], min(terminal->columns, columns)*sizeof(buffer_char_t));
      if (terminal->views[i].overflows)
        memcpy(overflows, terminal->views[i].overflows, max_lines * sizeof(int));
      free(terminal->views[i].buffer);
      free(terminal->views[i].overflows);
    }
    terminal->views[i].buffer = buffer;
    terminal->views[i].cursor_x = min(terminal->views[i].cursor_x, columns - 1);
    terminal->views[i].cursor_y = min(terminal->views[i].cursor_y, lines - 1);
    terminal->views[i].saved_cursor_x = min(terminal->views[i].saved_cursor_x, columns - 1);
    terminal->views[i].saved_cursor_y = min(terminal->views[i].saved_cursor_y, lines - 1);
    if (terminal->views[i].scrolling_region_end != -1 || terminal->views[i].scrolling_region_end != -1) {
      terminal->views[i].scrolling_region_start = min(terminal->views[i].scrolling_region_start, lines - 1);
      terminal->views[i].scrolling_region_end = min(terminal->views[i].scrolling_region_end, lines);
    }
    terminal->views[i].overflows = overflows;
  }
  terminal->columns = columns;
  terminal->lines = lines;
}

static char error_step[64];
static int set_error_step(const char* step) { strncpy(error_step, step, sizeof(error_step)); return 1; }
#if _WIN32
  long long last_error_code;
  static const char* terminal_get_last_error() {
    static char error_buffer[2048];
    strcpy(error_buffer, error_step);
    int len = strlen(error_buffer);
    error_buffer[len++] = ':';
    error_buffer[len++] = ' ';
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, last_error_code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&error_buffer[len], sizeof(error_buffer) - (len + 1), NULL);
    return error_buffer;
  }
#else
  // TODO, non-windows error handling, but less important because windows will be failing lots more, 'cause it's shit.
  static const char* terminal_get_last_error() { return error_step; }
#endif

static terminal_t* terminal_new(int columns, int lines, int scrollback_limit, const char* term_env, const char* pathname, const char** argv, const char** environment, const char* cwd) {
  terminal_t* terminal = calloc(sizeof(terminal_t), 1);
  for (int i = 0; i < VIEW_MAX; ++i) {
    for (int j = 0; j < 256; ++j)
      terminal->views[i].palette[j] = indexed_color(j);
    terminal->views[i].scrolling_region_end = -1;
    terminal->views[i].scrolling_region_start = -1;
    terminal->views[i].cursor_styling = LIBTERMINAL_NO_STYLING;
    terminal->views[i].saved_cursor_styling = LIBTERMINAL_NO_STYLING;
    terminal->views[i].saved_charset = CHARSET_US;
    terminal->views[i].tab_size = LIBTERMINAL_DEFAULT_TAB_SIZE;
  }
  terminal->scrollback_limit = scrollback_limit;
  terminal->mode = pathname && strcmp(pathname, "DUMMY") != 0 ? MODE_PTY : MODE_DUMMY;
  if (terminal->mode == MODE_PTY) {
    #ifdef _WIN32
      last_error_code = 0;
      HRESULT result = S_OK;
      SECURITY_ATTRIBUTES no_sec = { .nLength = sizeof(SECURITY_ATTRIBUTES), .bInheritHandle = TRUE, .lpSecurityDescriptor = NULL };
      HANDLE out_pipe_pseudo_console_side, in_pipe_pseudo_console_side;
      COORD size = { columns, lines };
      if ((!CreatePipe(&in_pipe_pseudo_console_side, &terminal->topty, &no_sec, 0) || !CreatePipe(&terminal->frompty, &out_pipe_pseudo_console_side, &no_sec, 0)) && set_error_step("create pipes"))
        goto error;
      result = CreatePseudoConsole(size, in_pipe_pseudo_console_side, out_pipe_pseudo_console_side, 0, &terminal->hpcon);
      if (FAILED(result) && set_error_step("create pseudoconsole"))
        goto error;
      terminal->nonblocking_buffer_mutex = CreateMutex(NULL, FALSE, NULL);
      if (!terminal->nonblocking_buffer_mutex && set_error_step("create mutex"))
        goto error;

      HANDLE handles_to_inherit[] = { in_pipe_pseudo_console_side, out_pipe_pseudo_console_side };
      STARTUPINFOEXW si_ex = {0};
      si_ex.StartupInfo.cb = sizeof(STARTUPINFOEXW);
      si_ex.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
      si_ex.StartupInfo.hStdInput = NULL;
      si_ex.StartupInfo.hStdOutput = NULL;
      si_ex.StartupInfo.hStdError = NULL;
      size_t list_size;
      // Create the appropriately sized thread attribute list
      InitializeProcThreadAttributeList(NULL, 2, 0, &list_size);
      si_ex.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(list_size);
      BOOL success = InitializeProcThreadAttributeList(si_ex.lpAttributeList, 2, 0, (PSIZE_T)&list_size) &&
        UpdateProcThreadAttribute(si_ex.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, terminal->hpcon, sizeof(HPCON), NULL, NULL) &&
        UpdateProcThreadAttribute(si_ex.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles_to_inherit, sizeof(handles_to_inherit), NULL, NULL);
      if (!success && set_error_step("update proc attribute list")) {
        DeleteProcThreadAttributeList(si_ex.lpAttributeList);
        free(si_ex.lpAttributeList);
        goto error;
      }

      int len = MultiByteToWideChar(CP_UTF8, 0, pathname, -1, NULL, 0);
      wchar_t* commandline = malloc(sizeof(wchar_t)*(len+1));
      len = MultiByteToWideChar(CP_UTF8, 0, pathname, -1, commandline, len);
      wchar_t* working_directory = NULL;
      if (cwd) {
        int cwd_len = MultiByteToWideChar(CP_UTF8, 0, cwd, -1, NULL, 0);
        working_directory = malloc(sizeof(wchar_t) * cwd_len);
        MultiByteToWideChar(CP_UTF8, 0, cwd, -1, working_directory, cwd_len);
      }
      success = CreateProcessW(NULL, commandline, NULL, NULL, TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT, (void*)environment[0], working_directory, &si_ex.StartupInfo, &terminal->process_information);
      free(working_directory);
      DeleteProcThreadAttributeList(si_ex.lpAttributeList);
      free(si_ex.lpAttributeList);
      free(commandline);
      if (!success && set_error_step("create process"))
        goto error;
      terminal->nonblocking_thread = CreateThread(NULL, 0, windows_nonblocking_thread_callback, terminal, 0, NULL);
      if (!terminal->nonblocking_thread && set_error_step("create thread"))
        goto error;
      error:
      if (!terminal->nonblocking_thread) {
        last_error_code = FAILED(result) ? HRESULT_CODE(result) : GetLastError();
        terminal_close(terminal);
        free(terminal);
        return NULL;
      }
    #else
      struct termios term = {0};
      term.c_cc[VINTR] = 3;
      term.c_cc[VSTART] = '\x13';
      term.c_cc[VSTOP] = '\x11';
      term.c_cc[VSUSP] = 26;
      term.c_cc[VERASE] = '\x7F';
      term.c_cc[VEOL] = 0;
      term.c_cc[VEOF] = 4;
      term.c_lflag |= ISIG | ECHO | ICANON | IEXTEN | ECHOE | ECHOK | ECHOCTL | ECHOKE;
      term.c_cflag |= CS8 | CREAD;
      term.c_iflag |= IUTF8 | ICRNL | IXON;
      term.c_oflag |= OPOST | ONLCR | NL0 | CR0 | TAB0 | BS0 | VT0 | FF0;
      struct winsize size = { .ws_row = lines, .ws_col = columns, .ws_xpixel = 0, .ws_ypixel = 0 };
      terminal->pid = forkpty(&terminal->master, NULL, &term, &size);
      if (terminal->pid == -1 && set_error_step("forkpty")) {
        free(terminal);
        return NULL;
      }
      if (!terminal->pid) {
        if (cwd && chdir(cwd) != 0)
          _exit(127);
        setenv("TERM", term_env, 1);
        for (int i = 0; i < 256 && environment[i]; i += 2)
          setenv(environment[i], environment[i+1], 1);
        execvp(pathname,  (char** const)argv);
        exit(-1);
        return NULL;
      }
      int flags = fcntl(terminal->master, F_GETFD, 0);
      fcntl(terminal->master, F_SETFL, flags | O_NONBLOCK);
    #endif
  }
  terminal_resize(terminal, columns, lines);
  return terminal;
}




/* Public editor-independent core bridge.  The implementation is kept here
 * during the extraction phase so the Lua ABI and native users share exactly
 * the same parser/runtime state. */
terminal_t* terminal_core_new(int columns, int lines, int scrollback_limit,
    const char* term_env, const char* pathname, const char** argv,
    const char** environment, const char* cwd) {
  return terminal_new(columns, lines, scrollback_limit, term_env, pathname,
    argv, environment, cwd);
}

void terminal_core_free(terminal_t* terminal) { terminal_free(terminal); }
const char* terminal_core_last_error(void) { return terminal_get_last_error(); }
int terminal_core_close(terminal_t* terminal) { return terminal_close(terminal); }
void terminal_core_set_debug(terminal_t* terminal, int enabled) {
  if (terminal)
    terminal->debug = enabled;
}
int terminal_core_feed(terminal_t* terminal, const char* data, int length) {
  return terminal_input(terminal, data, length);
}
void terminal_core_clear_scrollback(terminal_t* terminal) {
  terminal_clear_scrollback_buffer(terminal);
}
void terminal_core_clear(terminal_t* terminal) {
  if (!terminal || terminal->closed)
    return;
  terminal_clear_scrollback_buffer(terminal);
  view_t* view = &terminal->views[terminal->current_view];
  if (view->buffer)
    memset(view->buffer, 0, sizeof(buffer_char_t) * terminal->columns * terminal->lines);
  if (view->overflows)
    memset(view->overflows, 0, sizeof(int) * terminal->lines);
  view->cursor_x = 0;
  view->cursor_y = 0;
}
void terminal_core_reset(terminal_t* terminal) {
  if (!terminal)
    return;
  if (terminal->closed)
    return;
  terminal_clear_scrollback_buffer(terminal);
  terminal->current_view = VIEW_NORMAL_BUFFER;
  terminal->paste_mode = PASTE_NORMAL;
  terminal->mouse_tracking_mode = MOUSE_TRACKING_NONE;
  terminal->mouse_encoding = MOUSE_ENCODING_DEFAULT;
  terminal->reporting_focus = 0;
  terminal->name[0] = 0;
  terminal->buffered_sequence[0] = 0;
  for (int i = 0; i < VIEW_MAX; ++i) {
    view_t* view = &terminal->views[i];
    if (view->buffer)
      memset(view->buffer, 0, sizeof(buffer_char_t) * terminal->columns * terminal->lines);
    if (view->overflows)
      memset(view->overflows, 0, sizeof(int) * terminal->lines);
    view->cursor_x = 0;
    view->cursor_y = 0;
    view->cursor_styling_inversed = 0;
    view->cursor_styling = LIBTERMINAL_NO_STYLING;
    view->saved_cursor_x = 0;
    view->saved_cursor_y = 0;
    view->saved_cursor_styling_inversed = 0;
    view->saved_cursor_styling = LIBTERMINAL_NO_STYLING;
    view->saved_charset = CHARSET_US;
    view->cursor_mode = CURSOR_SOLID;
    view->cursor_keys_mode = KEYS_MODE_NORMAL;
    view->keypad_keys_mode = KEYS_MODE_NORMAL;
    view->charset = CHARSET_US;
    view->last_graphical_character = 0;
    view->scrolling_region_start = -1;
    view->scrolling_region_end = -1;
    view->tab_size = LIBTERMINAL_DEFAULT_TAB_SIZE;
    for (int color = 0; color < 256; ++color)
      view->palette[color] = indexed_color(color);
  }
}
int terminal_core_update(terminal_t* terminal, terminal_core_output_callback callback,
    void* data, int* total_shifts) {
  return terminal_update(terminal, callback, data, total_shifts);
}
void terminal_core_resize(terminal_t* terminal, int columns, int lines) {
  if (!terminal->closed)
    terminal_resize(terminal, columns, lines);
}
int terminal_core_is_closed(terminal_t* terminal) { return terminal->closed; }
void terminal_core_dimensions(terminal_t* terminal, int* columns, int* lines) {
  if (!terminal)
    return;
  if (columns) *columns = terminal->columns;
  if (lines) *lines = terminal->lines;
}
void terminal_core_cursor(terminal_t* terminal, int* column, int* row, int* mode) {
  if (!terminal)
    return;
  view_t* view = &terminal->views[terminal->current_view];
  if (column) *column = view->cursor_x;
  if (row) *row = view->cursor_y;
  if (mode) *mode = view->cursor_mode;
}
void terminal_core_modes(terminal_t* terminal, int* cursor_keys_mode,
    int* keypad_keys_mode, int* mouse_tracking_mode, int* mouse_encoding,
    int* paste_mode, int* reporting_focus) {
  if (!terminal)
    return;
  view_t* view = &terminal->views[terminal->current_view];
  if (cursor_keys_mode) *cursor_keys_mode = view->cursor_keys_mode;
  if (keypad_keys_mode) *keypad_keys_mode = view->keypad_keys_mode;
  if (mouse_tracking_mode) *mouse_tracking_mode = terminal->mouse_tracking_mode;
  if (mouse_encoding) *mouse_encoding = terminal->mouse_encoding;
  if (paste_mode) *paste_mode = terminal->paste_mode;
  if (reporting_focus) *reporting_focus = terminal->reporting_focus;
}
const char* terminal_core_name(terminal_t* terminal) {
  if (!terminal || !terminal->name[0])
    return NULL;
  return terminal->name;
}
void terminal_core_focus(terminal_t* terminal, int focused) {
  if (terminal && terminal->reporting_focus)
    terminal_input(terminal, focused ? "\x1B[" : "\x1B[O", 3);
}
void terminal_core_scrollback(terminal_t* terminal, int position,
    int* current, int* total) {
  if (!terminal)
    return;
  if (terminal->current_view == VIEW_NORMAL_BUFFER && position >= 0)
    terminal_scrollback(terminal, position);
  if (current) *current = terminal->current_view == VIEW_NORMAL_BUFFER
    ? terminal->scrollback_position : 0;
  if (total) *total = terminal->current_view == VIEW_NORMAL_BUFFER
    ? terminal->scrollback_total_lines : 0;
}
static int terminal_core_emit_line(terminal_core_line_callback callback,
    void* user_data, int row, const buffer_char_t* line, int columns,
    int overflow) {
  if (!callback || !line || columns <= 0)
    return 0;
  char* text = malloc((size_t)columns * 4 + 1);
  if (!text)
    return 0;
  int column = 0;
  while (column < columns) {
    uint64_t style = line[column].styling.value;
    int length = 0;
    while (column < columns && line[column].styling.value == style) {
      unsigned int codepoint = line[column].codepoint;
      length += codepoint_to_utf8(codepoint ? codepoint : ' ', &text[length]);
      ++column;
    }
    callback(row, style, text, length, overflow, user_data);
  }
  free(text);
  return 1;
}

int terminal_core_for_each_line(terminal_t* terminal, int first_row,
    int last_row, terminal_core_line_callback callback, void* user_data) {
  if (!terminal || terminal->closed || !callback || first_row > last_row)
    return 0;

  view_t* view = &terminal->views[terminal->current_view];
  int requested = last_row - first_row + 1;
  int emitted = 0;

  if (terminal->current_view == VIEW_NORMAL_BUFFER && first_row < 0) {
    int offset = -first_row;
    int top_offset = terminal->scrollback_target_top_offset;
    backbuffer_page_t* page = terminal_find_scrollback_page(
      terminal, terminal->scrollback_target, &offset, &top_offset);
    int lines_into_page = top_offset - offset;
    while (page && emitted < requested) {
      int* overflows = (int*)&page->buffer[
        LIBTERMINAL_BACKBUFFER_PAGE_LINES * page->columns];
      for (int row = lines_into_page; row < page->line && emitted < requested; ++row) {
        terminal_core_emit_line(callback, user_data, first_row + emitted,
          &page->buffer[row * page->columns], page->columns, overflows[row]);
        ++emitted;
      }
      page = page->next;
      lines_into_page = 0;
    }
  }

  int active_row = max(first_row, 0);
  while (active_row < terminal->lines && emitted < requested) {
    terminal_core_emit_line(callback, user_data, first_row + emitted,
      &view->buffer[active_row * terminal->columns], terminal->columns,
      view->overflows[active_row]);
    ++active_row;
    ++emitted;
  }
  return emitted;
}
int terminal_core_exited(terminal_t* terminal, int* exit_code, int* signal) {
  if (terminal->closed || terminal->mode != MODE_PTY) return 0;
#if _WIN32
  DWORD code;
  if (!GetExitCodeProcess(terminal->process_information.hProcess, &code)
      || code == STILL_ACTIVE)
    return 0;
  if (exit_code) *exit_code = (int)code;
  if (signal) *signal = -1;
  return 1;
#else
  int status;
  if (waitpid(terminal->pid, &status, WNOHANG) <= 0)
    return 0;
  if (exit_code) *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  if (signal) *signal = WIFSIGNALED(status) ? WTERMSIG(status) : -1;
  return 1;
#endif
}
