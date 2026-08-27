#include "terminal_emulator.h"

#include <libtsm.h>

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TERMINAL_NAME_MAX 256
#define TERMINAL_CHECKPOINT_VERSION 1u
#define TERMINAL_CHECKPOINT_HEADER_SIZE 32u
#define TERMINAL_CHECKPOINT_MAX_HISTORY (256u * 1024u * 1024u)

enum {
  TERMINAL_EMULATOR_CURSOR_SOLID = 0,
  TERMINAL_EMULATOR_CURSOR_HIDDEN = 1,
  TERMINAL_EMULATOR_CURSOR_BLINKING = 2,
  TERMINAL_EMULATOR_KEYS_NORMAL = 0,
  TERMINAL_EMULATOR_KEYS_APPLICATION = 1,
  TERMINAL_EMULATOR_MOUSE_NONE = 0,
  TERMINAL_EMULATOR_MOUSE_X10 = 1,
  TERMINAL_EMULATOR_MOUSE_NORMAL = 2,
  TERMINAL_EMULATOR_MOUSE_BUTTON = 3,
  TERMINAL_EMULATOR_MOUSE_ANY = 4,
  TERMINAL_EMULATOR_MOUSE_ENCODING_DEFAULT = 0,
  TERMINAL_EMULATOR_MOUSE_ENCODING_SGR = 1,
  TERMINAL_EMULATOR_PASTE_NORMAL = 0,
  TERMINAL_EMULATOR_PASTE_BRACKETED = 1
};

static const uint8_t terminal_checkpoint_magic[8] = {
  'P', 'T', 'S', 'M', 'C', 'P', 0, 0
};

enum terminal_history_event {
  TERMINAL_HISTORY_FEED = 1,
  TERMINAL_HISTORY_RESIZE,
  TERMINAL_HISTORY_SCROLLBACK,
  TERMINAL_HISTORY_CLEAR_SCROLLBACK,
  TERMINAL_HISTORY_CLEAR,
  TERMINAL_HISTORY_RESET
};

typedef union terminal_color {
  struct {
    uint8_t attributes;
    union {
      struct {
        uint8_t r;
        uint8_t g;
        uint8_t b;
      } rgb;
      uint8_t index;
    } value;
  } parts;
  uint32_t value;
} terminal_color_t;

#define TERMINAL_ATTRIBUTE_UNSET_COLOR 0
#define TERMINAL_ATTRIBUTE_INVERSE_COLOR 1
#define TERMINAL_ATTRIBUTE_INDEX_COLOR 2
#define TERMINAL_ATTRIBUTE_RGB_COLOR 3
#define TERMINAL_ATTRIBUTE_BOLD 8
#define TERMINAL_ATTRIBUTE_ITALIC 16
#define TERMINAL_ATTRIBUTE_UNDERLINE 32

typedef struct terminal_history {
  uint8_t* data;
  size_t length;
  size_t capacity;
} terminal_history_t;

typedef struct terminal_emulator {
  struct tsm_screen* screen;
  struct tsm_vte* vte;
  int columns;
  int rows;
  int scrollback_limit;
  int closed;
  int debug;
  char name[TERMINAL_NAME_MAX];
  terminal_history_t history;
  terminal_emulator_input_callback input_callback;
  void* input_callback_data;
  int replaying;
} terminal_t;

static int terminal_emulator_feed_internal(terminal_t* terminal,
    const char* data, size_t length, int record);

static void write_callback(struct tsm_vte* vte, const char* data, size_t length,
    void* user_data) {
  (void)vte;
  terminal_t* terminal = (terminal_t*)user_data;
  if (!terminal || terminal->closed || !data || !length)
    return;
  if (terminal->replaying)
    return;
  if (terminal->input_callback) {
    terminal->input_callback(data, (int)length, terminal->input_callback_data);
  } else {
    terminal_emulator_feed_internal(terminal, data, length, 0);
  }
}

static void osc_callback(struct tsm_vte* vte, const char* data, size_t length,
    void* user_data) {
  (void)vte;
  terminal_t* terminal = (terminal_t*)user_data;
  if (!terminal || !data || length < 2)
    return;

  size_t separator = 0;
  while (separator < length && data[separator] != ';')
    ++separator;
  if (separator == 0 || separator >= length ||
      (data[0] != '0' && data[0] != '2'))
    return;

  size_t title_length = length - separator - 1;
  if (title_length >= sizeof(terminal->name))
    title_length = sizeof(terminal->name) - 1;
  memcpy(terminal->name, &data[separator + 1], title_length);
  terminal->name[title_length] = 0;
}

static void mouse_callback(struct tsm_vte* vte,
    enum tsm_mouse_track_mode track_mode, bool track_pixels, void* user_data) {
  (void)vte;
  (void)track_mode;
  (void)track_pixels;
  (void)user_data;
}

static void write_u32(uint8_t* data, uint32_t value) {
  data[0] = (uint8_t)(value >> 0);
  data[1] = (uint8_t)(value >> 8);
  data[2] = (uint8_t)(value >> 16);
  data[3] = (uint8_t)(value >> 24);
}

static uint32_t read_u32(const uint8_t* data) {
  return ((uint32_t)data[0] << 0)
    | ((uint32_t)data[1] << 8)
    | ((uint32_t)data[2] << 16)
    | ((uint32_t)data[3] << 24);
}

static int history_reserve(terminal_history_t* history, size_t amount) {
  if (amount > TERMINAL_CHECKPOINT_MAX_HISTORY)
    return 0;
  if (amount <= history->capacity)
    return 1;

  size_t capacity = history->capacity ? history->capacity : 4096;
  while (capacity < amount) {
    if (capacity > TERMINAL_CHECKPOINT_MAX_HISTORY / 2) {
      capacity = TERMINAL_CHECKPOINT_MAX_HISTORY;
      break;
    }
    capacity *= 2;
  }
  uint8_t* data = realloc(history->data, capacity);
  if (!data)
    return 0;
  history->data = data;
  history->capacity = capacity;
  return 1;
}

static int history_append(terminal_history_t* history, unsigned int type,
    const void* data, size_t length) {
  if (length > UINT32_MAX || history->length > SIZE_MAX - 5 - length)
    return 0;
  size_t required = history->length + 5 + length;
  if (!history_reserve(history, required))
    return 0;
  history->data[history->length] = (uint8_t)type;
  write_u32(&history->data[history->length + 1], (uint32_t)length);
  if (length)
    memcpy(&history->data[history->length + 5], data, length);
  history->length = required;
  return 1;
}

static int history_append_u32(terminal_history_t* history, unsigned int type,
    uint32_t value) {
  uint8_t data[4];
  write_u32(data, value);
  return history_append(history, type, data, sizeof(data));
}

static int history_append_resize(terminal_history_t* history, int columns,
    int rows) {
  uint8_t data[8];
  write_u32(&data[0], (uint32_t)columns);
  write_u32(&data[4], (uint32_t)rows);
  return history_append(history, TERMINAL_HISTORY_RESIZE, data, sizeof(data));
}

static int history_valid(const uint8_t* data, size_t length) {
  size_t offset = 0;
  while (offset < length) {
    if (length - offset < 5)
      return 0;
    unsigned int type = data[offset];
    uint32_t event_length = read_u32(&data[offset + 1]);
    offset += 5;
    if ((size_t)event_length > length - offset)
      return 0;
    switch (type) {
      case TERMINAL_HISTORY_FEED:
        break;
      case TERMINAL_HISTORY_RESIZE:
        if (event_length != 8 || !read_u32(&data[offset])
            || !read_u32(&data[offset + 4]))
          return 0;
        break;
      case TERMINAL_HISTORY_SCROLLBACK:
        if (event_length != 4)
          return 0;
        break;
      case TERMINAL_HISTORY_CLEAR_SCROLLBACK:
      case TERMINAL_HISTORY_CLEAR:
      case TERMINAL_HISTORY_RESET:
        if (event_length != 0)
          return 0;
        break;
      default:
        return 0;
    }
    offset += event_length;
  }
  return offset == length;
}

static unsigned int current_scrollback(const terminal_t* terminal) {
  unsigned int count = tsm_screen_sb_get_line_count(terminal->screen);
  unsigned int position = tsm_screen_sb_get_line_pos(terminal->screen);
  return position > count ? 0 : count - position;
}

static void set_scrollback(terminal_t* terminal, unsigned int target) {
  unsigned int count = tsm_screen_sb_get_line_count(terminal->screen);
  if (target > count)
    target = count;
  unsigned int current = current_scrollback(terminal);
  if (target > current)
    tsm_screen_sb_up(terminal->screen, target - current);
  else if (current > target)
    tsm_screen_sb_down(terminal->screen, current - target);
}

static int estimate_shifts(terminal_t* terminal, const uint64_t* before,
    unsigned int before_count, unsigned int old_scrollback) {
  unsigned int after_scrollback = tsm_screen_sb_get_line_count(terminal->screen);
  int shifts = after_scrollback > old_scrollback
    ? (int)(after_scrollback - old_scrollback) : 0;
  unsigned int rows = (unsigned int)terminal->rows;
  if (!before || before_count != rows || rows < 2)
    return shifts;

  uint64_t* after = malloc(sizeof(*after) * rows);
  if (!after)
    return shifts;
  for (unsigned int row = 0; row < rows; ++row)
    after[row] = tsm_screen_get_row_id(terminal->screen, row);

  unsigned int best_matches = 0;
  unsigned int best_shift = 0;
  for (unsigned int shift = 1; shift < rows; ++shift) {
    unsigned int matches = 0;
    for (unsigned int row = 0; row + shift < rows; ++row)
      if (after[row] && after[row] == before[row + shift])
        ++matches;
    if (matches > best_matches) {
      best_matches = matches;
      best_shift = shift;
    }
  }
  free(after);
  if (best_matches >= 2 && (int)best_shift > shifts)
    shifts = (int)best_shift;
  return shifts;
}

static int terminal_emulator_feed_internal(terminal_t* terminal,
    const char* data, size_t length, int record) {
  if (!terminal || terminal->closed || !data || length > INT_MAX)
    return 0;

  unsigned int rows = (unsigned int)terminal->rows;
  uint64_t* before = rows ? malloc(sizeof(*before) * rows) : NULL;
  if (before) {
    for (unsigned int row = 0; row < rows; ++row)
      before[row] = tsm_screen_get_row_id(terminal->screen, row);
  }
  unsigned int old_scrollback = tsm_screen_sb_get_line_count(terminal->screen);

  if (record && !terminal->replaying
      && !history_append(&terminal->history, TERMINAL_HISTORY_FEED,
        data, length)) {
    free(before);
    return 0;
  }
  tsm_vte_input(terminal->vte, data, length);
  int shifts = estimate_shifts(terminal, before, rows, old_scrollback);
  free(before);
  return shifts;
}

typedef struct terminal_draw_context {
  terminal_t* terminal;
  terminal_emulator_line_callback callback;
  void* user_data;
  unsigned int scrollback;
  int first_row;
  int last_row;
  int emitted;
  int pending;
  int pending_row;
  uint64_t pending_style;
  char* pending_text;
  size_t pending_length;
  size_t pending_capacity;
  int last_started_row;
} terminal_draw_context_t;

static void flush_pending_line(terminal_draw_context_t* context) {
  if (!context->pending)
    return;
  context->callback(context->pending_row, context->pending_style,
    context->pending_text, (int)context->pending_length, 0,
    context->user_data);
  context->pending = 0;
  context->pending_length = 0;
}

static terminal_color_t color_from_attr(const terminal_t* terminal, int8_t code,
    uint8_t r, uint8_t g, uint8_t b, int foreground) {
  terminal_color_t color = {0};
  if ((foreground && code == TSM_COLOR_FOREGROUND)
      || (!foreground && code == TSM_COLOR_BACKGROUND)) {
    color.parts.attributes = TERMINAL_ATTRIBUTE_UNSET_COLOR;
  } else if (code >= 0 && code < TSM_COLOR_NUM
      && !(code < 16 && tsm_vte_get_palette((struct tsm_vte*)terminal->vte)
        && !strcmp(tsm_vte_get_palette((struct tsm_vte*)terminal->vte), "custom"))) {
    color.parts.attributes = TERMINAL_ATTRIBUTE_INDEX_COLOR;
    color.parts.value.index = (uint8_t)code;
  } else {
    color.parts.attributes = TERMINAL_ATTRIBUTE_RGB_COLOR;
    color.parts.value.rgb.r = r;
    color.parts.value.rgb.g = g;
    color.parts.value.rgb.b = b;
  }
  return color;
}

static uint64_t style_from_attr(const terminal_t* terminal,
    const struct tsm_screen_attr* attr) {
  terminal_color_t foreground = color_from_attr(terminal, attr->fccode, attr->fr,
    attr->fg, attr->fb, 1);
  terminal_color_t background = color_from_attr(terminal, attr->bccode, attr->br,
    attr->bg, attr->bb, 0);
  if (attr->inverse) {
    terminal_color_t swapped = foreground;
    foreground = background;
    background = swapped;
  }
  if (attr->bold)
    foreground.parts.attributes |= TERMINAL_ATTRIBUTE_BOLD;
  if (attr->italic)
    foreground.parts.attributes |= TERMINAL_ATTRIBUTE_ITALIC;
  if (attr->underline)
    foreground.parts.attributes |= TERMINAL_ATTRIBUTE_UNDERLINE;
  return ((uint64_t)background.value << 32) | foreground.value;
}

static int draw_cell(struct tsm_screen* screen, uint64_t id,
    const uint32_t* chars, size_t length, unsigned int width,
    unsigned int posx, unsigned int posy, const struct tsm_screen_attr* attr,
    tsm_age_t age, void* user_data) {
  (void)screen;
  (void)id;
  (void)posx;
  (void)age;
  terminal_draw_context_t* context = (terminal_draw_context_t*)user_data;
  int row = (int)posy - (int)context->scrollback;
  if (context->pending && row != context->pending_row)
    flush_pending_line(context);
  if (row < context->first_row || row > context->last_row)
    return 0;

  char text[64];
  size_t text_length = 0;
  text[0] = 0;
  if (length) {
    for (size_t i = 0; i < length && text_length + 4 < sizeof(text); ++i)
      text_length += tsm_ucs4_to_utf8(chars[i], &text[text_length]);
  } else if (width) {
    text[0] = ' ';
    text_length = 1;
  }
  uint64_t style = style_from_attr(context->terminal, attr);
  if (!context->pending || context->pending_style != style) {
    flush_pending_line(context);
    context->pending = 1;
    context->pending_row = row;
    context->pending_style = style;
    if (context->last_started_row != row) {
      ++context->emitted;
      context->last_started_row = row;
    }
  }
  if (text_length > context->pending_capacity - context->pending_length) {
    flush_pending_line(context);
    context->pending = 1;
    context->pending_row = row;
    context->pending_style = style;
  }
  if (text_length <= context->pending_capacity - context->pending_length) {
    memcpy(&context->pending_text[context->pending_length], text,
      text_length);
    context->pending_length += text_length;
  }
  return 0;
}

static void terminal_emulator_clear_internal(terminal_t* terminal) {
  tsm_screen_clear_sb(terminal->screen);
  tsm_screen_erase_screen(terminal->screen, false);
  tsm_screen_move_to(terminal->screen, 0, 0);
  terminal->name[0] = 0;
}

static void terminal_emulator_reset_internal(terminal_t* terminal) {
  tsm_vte_hard_reset(terminal->vte);
  terminal->name[0] = 0;
}

static int replay_history(terminal_t* terminal, const uint8_t* data,
    size_t length) {
  size_t offset = 0;
  terminal->replaying = 1;
  while (offset < length) {
    unsigned int type = data[offset++];
    uint32_t event_length = read_u32(&data[offset]);
    offset += 4;
    const uint8_t* payload = &data[offset];
    offset += event_length;
    switch (type) {
      case TERMINAL_HISTORY_FEED:
        terminal_emulator_feed_internal(terminal, (const char*)payload,
          event_length, 0);
        break;
      case TERMINAL_HISTORY_RESIZE:
        terminal->columns = (int)read_u32(payload);
        terminal->rows = (int)read_u32(&payload[4]);
        if (tsm_screen_resize(terminal->screen, (unsigned int)terminal->columns,
            (unsigned int)terminal->rows)) {
          terminal->replaying = 0;
          return 0;
        }
        break;
      case TERMINAL_HISTORY_SCROLLBACK:
        set_scrollback(terminal, read_u32(payload));
        break;
      case TERMINAL_HISTORY_CLEAR_SCROLLBACK:
        tsm_screen_clear_sb(terminal->screen);
        break;
      case TERMINAL_HISTORY_CLEAR:
        terminal_emulator_clear_internal(terminal);
        break;
      case TERMINAL_HISTORY_RESET:
        terminal_emulator_reset_internal(terminal);
        break;
    }
  }
  terminal->replaying = 0;
  return 1;
}

terminal_emulator_t* terminal_emulator_new(
    int columns, int rows, int scrollback_limit, const char* term) {
  (void)term;
  if (columns <= 0 || rows <= 0 || scrollback_limit < 0)
    return NULL;
  terminal_t* terminal = calloc(1, sizeof(*terminal));
  if (!terminal)
    return NULL;
  if (tsm_screen_new(&terminal->screen, NULL, NULL)
      || tsm_screen_resize(terminal->screen, (unsigned int)columns,
        (unsigned int)rows)) {
    tsm_screen_unref(terminal->screen);
    free(terminal);
    return NULL;
  }
  terminal->columns = columns;
  terminal->rows = rows;
  terminal->scrollback_limit = scrollback_limit;
  tsm_screen_set_max_sb(terminal->screen, (unsigned int)scrollback_limit);
  if (tsm_vte_new(&terminal->vte, terminal->screen, write_callback, terminal,
      NULL, NULL)) {
    tsm_screen_unref(terminal->screen);
    free(terminal);
    return NULL;
  }
  tsm_vte_set_osc_cb(terminal->vte, osc_callback, terminal);
  tsm_vte_set_mouse_cb(terminal->vte, mouse_callback, terminal);
  return (terminal_emulator_t*)terminal;
}

void terminal_emulator_free(terminal_emulator_t* emulator) {
  terminal_t* terminal = (terminal_t*)emulator;
  if (!terminal)
    return;
  if (!terminal->closed)
    terminal_emulator_close(emulator);
  free(terminal->history.data);
  free(terminal);
}

void terminal_emulator_set_debug(terminal_emulator_t* emulator, int enabled) {
  terminal_t* terminal = (terminal_t*)emulator;
  if (terminal)
    terminal->debug = enabled;
}

void terminal_emulator_set_input_callback(terminal_emulator_t* emulator,
    terminal_emulator_input_callback callback, void* user_data) {
  terminal_t* terminal = (terminal_t*)emulator;
  if (!terminal)
    return;
  terminal->input_callback = callback;
  terminal->input_callback_data = user_data;
}

int terminal_emulator_feed(terminal_emulator_t* emulator,
    const char* data, size_t length) {
  return terminal_emulator_feed_internal((terminal_t*)emulator, data, length, 1);
}

size_t terminal_emulator_checkpoint_size(terminal_emulator_t* emulator) {
  terminal_t* terminal = (terminal_t*)emulator;
  if (!terminal || terminal->closed || terminal->history.length
      > SIZE_MAX - TERMINAL_CHECKPOINT_HEADER_SIZE)
    return 0;
  return TERMINAL_CHECKPOINT_HEADER_SIZE + terminal->history.length;
}

int terminal_emulator_checkpoint(terminal_emulator_t* emulator, void* data,
    size_t size, size_t* written) {
  terminal_t* terminal = (terminal_t*)emulator;
  size_t required = terminal_emulator_checkpoint_size(emulator);
  if (written)
    *written = required;
  if (!terminal || !required || !data || size < required)
    return 0;
  uint8_t* output = (uint8_t*)data;
  memcpy(output, terminal_checkpoint_magic, sizeof(terminal_checkpoint_magic));
  write_u32(&output[8], TERMINAL_CHECKPOINT_VERSION);
  write_u32(&output[12], (uint32_t)terminal->columns);
  write_u32(&output[16], (uint32_t)terminal->rows);
  write_u32(&output[20], (uint32_t)terminal->scrollback_limit);
  write_u32(&output[24], (uint32_t)terminal->history.length);
  write_u32(&output[28], current_scrollback(terminal));
  memcpy(&output[TERMINAL_CHECKPOINT_HEADER_SIZE], terminal->history.data,
    terminal->history.length);
  return 1;
}

int terminal_emulator_restore_checkpoint(terminal_emulator_t* emulator,
    const void* data, size_t size) {
  terminal_t* terminal = (terminal_t*)emulator;
  if (!terminal || terminal->closed || !data
      || size < TERMINAL_CHECKPOINT_HEADER_SIZE)
    return 0;
  const uint8_t* input = (const uint8_t*)data;
  if (memcmp(input, terminal_checkpoint_magic, sizeof(terminal_checkpoint_magic))
      || read_u32(&input[8]) != TERMINAL_CHECKPOINT_VERSION)
    return 0;
  uint32_t columns = read_u32(&input[12]);
  uint32_t rows = read_u32(&input[16]);
  uint32_t scrollback_limit = read_u32(&input[20]);
  uint32_t history_length = read_u32(&input[24]);
  uint32_t scrollback = read_u32(&input[28]);
  if (!columns || !rows || columns > INT_MAX || rows > INT_MAX
      || scrollback_limit > INT_MAX
      || history_length > size - TERMINAL_CHECKPOINT_HEADER_SIZE
      || history_length > TERMINAL_CHECKPOINT_MAX_HISTORY
      || history_length != size - TERMINAL_CHECKPOINT_HEADER_SIZE
      || !history_valid(&input[TERMINAL_CHECKPOINT_HEADER_SIZE], history_length))
    return 0;

  uint8_t* history = malloc(history_length ? history_length : 1);
  if (!history)
    return 0;
  memcpy(history, &input[TERMINAL_CHECKPOINT_HEADER_SIZE], history_length);

  terminal->columns = (int)columns;
  terminal->rows = (int)rows;
  terminal->scrollback_limit = (int)scrollback_limit;
  terminal_emulator_reset_internal(terminal);
  tsm_screen_set_max_sb(terminal->screen, scrollback_limit);
  if (tsm_screen_resize(terminal->screen, columns, rows)
      || !replay_history(terminal, history, history_length)) {
    free(history);
    return 0;
  }
  set_scrollback(terminal, scrollback);
  free(terminal->history.data);
  terminal->history.data = history;
  terminal->history.length = history_length;
  terminal->history.capacity = history_length;
  return 1;
}

void terminal_emulator_resize(terminal_emulator_t* emulator,
    int columns, int rows) {
  terminal_t* terminal = (terminal_t*)emulator;
  if (!terminal || terminal->closed || columns <= 0 || rows <= 0)
    return;
  if (terminal->columns == columns && terminal->rows == rows)
    return;
  if (tsm_screen_resize(terminal->screen, (unsigned int)columns,
      (unsigned int)rows))
    return;
  terminal->columns = columns;
  terminal->rows = rows;
  if (!terminal->replaying)
    history_append_resize(&terminal->history, columns, rows);
}

int terminal_emulator_close(terminal_emulator_t* emulator) {
  terminal_t* terminal = (terminal_t*)emulator;
  if (!terminal || terminal->closed)
    return 0;
  terminal->closed = 1;
  tsm_vte_unref(terminal->vte);
  tsm_screen_unref(terminal->screen);
  terminal->vte = NULL;
  terminal->screen = NULL;
  return 0;
}

int terminal_emulator_is_closed(terminal_emulator_t* emulator) {
  terminal_t* terminal = (terminal_t*)emulator;
  return !terminal || terminal->closed;
}

void terminal_emulator_clear_scrollback(terminal_emulator_t* emulator) {
  terminal_t* terminal = (terminal_t*)emulator;
  if (!terminal || terminal->closed)
    return;
  tsm_screen_clear_sb(terminal->screen);
  if (!terminal->replaying)
    history_append(&terminal->history, TERMINAL_HISTORY_CLEAR_SCROLLBACK,
      NULL, 0);
}

void terminal_emulator_clear(terminal_emulator_t* emulator) {
  terminal_t* terminal = (terminal_t*)emulator;
  if (!terminal || terminal->closed)
    return;
  terminal_emulator_clear_internal(terminal);
  if (!terminal->replaying)
    history_append(&terminal->history, TERMINAL_HISTORY_CLEAR, NULL, 0);
}

void terminal_emulator_reset(terminal_emulator_t* emulator) {
  terminal_t* terminal = (terminal_t*)emulator;
  if (!terminal || terminal->closed)
    return;
  terminal_emulator_reset_internal(terminal);
  if (!terminal->replaying)
    history_append(&terminal->history, TERMINAL_HISTORY_RESET, NULL, 0);
}

void terminal_emulator_dimensions(terminal_emulator_t* emulator,
    int* columns, int* rows) {
  terminal_t* terminal = (terminal_t*)emulator;
  if (!terminal)
    return;
  if (columns) *columns = terminal->columns;
  if (rows) *rows = terminal->rows;
}

int terminal_emulator_cursor(terminal_emulator_t* emulator, int* column,
    int* row, int* mode) {
  terminal_t* terminal = (terminal_t*)emulator;
  if (!terminal || terminal->closed)
    return 0;
  if (column) *column = (int)tsm_screen_get_cursor_x(terminal->screen);
  if (row) *row = (int)tsm_screen_get_cursor_y(terminal->screen);
  if (mode) {
    unsigned int flags = tsm_screen_get_flags(terminal->screen);
    if (flags & TSM_SCREEN_HIDE_CURSOR)
      *mode = TERMINAL_EMULATOR_CURSOR_HIDDEN;
    else {
      enum tsm_screen_cursor_style style =
        tsm_screen_get_cursor_style(terminal->screen);
      *mode = style == TSM_SCREEN_CURSOR_DEFAULT
        || style == TSM_SCREEN_CURSOR_BLOCK_STEADY
        ? TERMINAL_EMULATOR_CURSOR_SOLID
        : TERMINAL_EMULATOR_CURSOR_BLINKING;
    }
  }
  return 1;
}

void terminal_emulator_modes(terminal_emulator_t* emulator,
    int* cursor_keys_mode, int* keypad_keys_mode, int* mouse_tracking_mode,
    int* mouse_encoding, int* paste_mode, int* reporting_focus) {
  terminal_t* terminal = (terminal_t*)emulator;
  if (!terminal || terminal->closed)
    return;
  unsigned int flags = tsm_vte_get_flags(terminal->vte);
  unsigned int mouse_mode = tsm_vte_get_mouse_mode(terminal->vte);
  unsigned int mouse_event = tsm_vte_get_mouse_event(terminal->vte);
  if (cursor_keys_mode)
    *cursor_keys_mode = flags & TSM_VTE_FLAG_CURSOR_KEY_MODE
      ? TERMINAL_EMULATOR_KEYS_APPLICATION : TERMINAL_EMULATOR_KEYS_NORMAL;
  if (keypad_keys_mode)
    *keypad_keys_mode = flags & TSM_VTE_FLAG_KEYPAD_APPLICATION_MODE
      ? TERMINAL_EMULATOR_KEYS_APPLICATION : TERMINAL_EMULATOR_KEYS_NORMAL;
  if (mouse_tracking_mode) {
    if (mouse_mode == TSM_VTE_MOUSE_MODE_X10)
      *mouse_tracking_mode = TERMINAL_EMULATOR_MOUSE_X10;
    else if (mouse_mode == TSM_VTE_MOUSE_MODE_VT200)
      *mouse_tracking_mode = TERMINAL_EMULATOR_MOUSE_NORMAL;
    else if (mouse_event == TSM_VTE_MOUSE_EVENT_BTN)
      *mouse_tracking_mode = TERMINAL_EMULATOR_MOUSE_BUTTON;
    else if (mouse_event == TSM_VTE_MOUSE_EVENT_ANY)
      *mouse_tracking_mode = TERMINAL_EMULATOR_MOUSE_ANY;
    else
      *mouse_tracking_mode = TERMINAL_EMULATOR_MOUSE_NONE;
  }
  if (mouse_encoding)
    *mouse_encoding = mouse_mode == TSM_VTE_MOUSE_MODE_SGR
      ? TERMINAL_EMULATOR_MOUSE_ENCODING_SGR
      : TERMINAL_EMULATOR_MOUSE_ENCODING_DEFAULT;
  if (paste_mode)
    *paste_mode = tsm_vte_get_bracketed_paste(terminal->vte)
      ? TERMINAL_EMULATOR_PASTE_BRACKETED : TERMINAL_EMULATOR_PASTE_NORMAL;
  if (reporting_focus)
    *reporting_focus = tsm_vte_get_focus_reporting(terminal->vte);
}

const char* terminal_emulator_name(terminal_emulator_t* emulator) {
  terminal_t* terminal = (terminal_t*)emulator;
  return terminal && terminal->name[0] ? terminal->name : NULL;
}

void terminal_emulator_focus(terminal_emulator_t* emulator, int focused) {
  terminal_t* terminal = (terminal_t*)emulator;
  if (terminal && !terminal->closed)
    tsm_vte_set_focus(terminal->vte, focused != 0);
}

void terminal_emulator_scrollback(terminal_emulator_t* emulator, int position,
    int* current, int* total) {
  terminal_t* terminal = (terminal_t*)emulator;
  if (!terminal || terminal->closed)
    return;
  if (position >= 0) {
    set_scrollback(terminal, (unsigned int)position);
    if (!terminal->replaying)
      history_append_u32(&terminal->history, TERMINAL_HISTORY_SCROLLBACK,
        (uint32_t)position);
  }
  if (current) *current = (int)current_scrollback(terminal);
  if (total) *total = (int)tsm_screen_sb_get_line_count(terminal->screen);
}

int terminal_emulator_for_each_line(terminal_emulator_t* emulator,
    int first_row, int last_row, terminal_emulator_line_callback callback,
    void* user_data) {
  terminal_t* terminal = (terminal_t*)emulator;
  if (!terminal || terminal->closed || !callback || first_row > last_row)
    return 0;
  terminal_draw_context_t context = {
    .terminal = terminal,
    .callback = callback,
    .user_data = user_data,
    .scrollback = current_scrollback(terminal),
    .first_row = first_row,
    .last_row = last_row,
    .last_started_row = INT_MIN,
  };
  context.pending_capacity = (size_t)terminal->columns * 40 + 1;
  context.pending_text = malloc(context.pending_capacity);
  if (!context.pending_text)
    return 0;
  tsm_screen_draw(terminal->screen, draw_cell, &context);
  flush_pending_line(&context);
  free(context.pending_text);
  int rows = last_row - first_row + 1;
  return context.emitted < rows ? context.emitted : rows;
}

int terminal_emulator_synchronized_output(terminal_emulator_t* emulator) {
  terminal_t* terminal = (terminal_t*)emulator;
  return terminal && !terminal->closed
    && tsm_vte_get_synchronized_output(terminal->vte);
}

int terminal_emulator_mouse(terminal_emulator_t* emulator,
    unsigned int cell_x, unsigned int cell_y, unsigned int button,
    unsigned int event, unsigned char modifiers) {
  terminal_t* terminal = (terminal_t*)emulator;
  if (!terminal || terminal->closed)
    return 0;
  if (button == 64)
    button = TSM_MOUSE_BUTTON_WHEEL_UP;
  else if (button == 65)
    button = TSM_MOUSE_BUTTON_WHEEL_DOWN;
  return tsm_vte_handle_mouse(terminal->vte, cell_x, cell_y, 0, 0,
    button, event, modifiers);
}
