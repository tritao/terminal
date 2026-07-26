#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include <string.h>
#include <math.h>
#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include "terminal_emulator.h"

#ifndef min
  static int min(int a, int b) { return a < b ? a : b; }
  static int max(int a, int b) { return a > b ? a : b; }
#endif

#define LIBTERMINAL_BACKBUFFER_PAGE_LINES 200
#define LIBTERMINAL_CHUNK_SIZE 4096
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

#define LIBTERMINAL_NO_STYLING ((buffer_styling_t) { \
  .foreground = UNSET_COLOR, .background = UNSET_COLOR \
})

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
  buffer_char_t buffer[1];
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

typedef void (*terminal_input_callback_t)(const char* data, int length, void* user_data);

typedef struct terminal_emulator {
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
  int closed;                                        // The emulator has been closed but the object remains alive.
  int reporting_focus;                               // Enables/disbles reporting focus.
  char name[LIBTERMINAL_NAME_MAX];                   // Window name, set with OS command.
  char buffered_sequence[LIBTERMINAL_CHUNK_SIZE];
  terminal_input_callback_t input_callback;
  void* input_callback_data;
} terminal_t;

static void terminal_clear_scrollback_buffer(terminal_t* terminal);

#define TERMINAL_CHECKPOINT_VERSION 1u
#define TERMINAL_CHECKPOINT_HEADER_SIZE 60u
#define TERMINAL_CHECKPOINT_MAX_CELLS (16u * 1024u * 1024u)
#define TERMINAL_CHECKPOINT_MAX_PAGES 100000u

static const uint8_t terminal_checkpoint_magic[8] = {
  'P', 'T', 'E', 'R', 'M', 'C', 'P', 0
};

static terminal_t* terminal_new(int columns, int lines, int scrollback_limit);
static void terminal_free(terminal_t* terminal);
static int terminal_scrollback(terminal_t* terminal, int target);

typedef struct terminal_checkpoint_writer {
  uint8_t* data;
  size_t size;
  size_t offset;
  int failed;
} terminal_checkpoint_writer_t;

typedef struct terminal_checkpoint_reader {
  const uint8_t* data;
  size_t size;
  size_t offset;
  int failed;
} terminal_checkpoint_reader_t;

static int checkpoint_add_size(size_t* size, size_t amount) {
  if (amount > SIZE_MAX - *size)
    return 0;
  *size += amount;
  return 1;
}

static int checkpoint_multiply_size(size_t* result, size_t left, size_t right) {
  if (left && right > SIZE_MAX / left)
    return 0;
  *result = left * right;
  return 1;
}

static size_t terminal_bounded_string_length(const char* string, size_t limit) {
  size_t length = 0;
  while (length < limit && string[length])
    ++length;
  return length;
}

static void checkpoint_write_bytes(terminal_checkpoint_writer_t* writer,
    const void* data, size_t length) {
  if (writer->failed || length > writer->size - writer->offset) {
    writer->failed = 1;
    return;
  }
  memcpy(&writer->data[writer->offset], data, length);
  writer->offset += length;
}

static void checkpoint_write_u32(terminal_checkpoint_writer_t* writer,
    uint32_t value) {
  uint8_t bytes[4] = {
    (uint8_t)(value >> 0), (uint8_t)(value >> 8),
    (uint8_t)(value >> 16), (uint8_t)(value >> 24)
  };
  checkpoint_write_bytes(writer, bytes, sizeof(bytes));
}

static void checkpoint_write_u64(terminal_checkpoint_writer_t* writer,
    uint64_t value) {
  uint8_t bytes[8] = {
    (uint8_t)(value >> 0), (uint8_t)(value >> 8),
    (uint8_t)(value >> 16), (uint8_t)(value >> 24),
    (uint8_t)(value >> 32), (uint8_t)(value >> 40),
    (uint8_t)(value >> 48), (uint8_t)(value >> 56)
  };
  checkpoint_write_bytes(writer, bytes, sizeof(bytes));
}

static void checkpoint_write_i32(terminal_checkpoint_writer_t* writer, int value) {
  checkpoint_write_u32(writer, (uint32_t)(int32_t)value);
}

static uint32_t checkpoint_read_u32(terminal_checkpoint_reader_t* reader) {
  if (reader->failed || sizeof(uint32_t) > reader->size - reader->offset) {
    reader->failed = 1;
    return 0;
  }
  const uint8_t* bytes = &reader->data[reader->offset];
  reader->offset += sizeof(uint32_t);
  return ((uint32_t)bytes[0] << 0)
    | ((uint32_t)bytes[1] << 8)
    | ((uint32_t)bytes[2] << 16)
    | ((uint32_t)bytes[3] << 24);
}

static uint64_t checkpoint_read_u64(terminal_checkpoint_reader_t* reader) {
  if (reader->failed || sizeof(uint64_t) > reader->size - reader->offset) {
    reader->failed = 1;
    return 0;
  }
  const uint8_t* bytes = &reader->data[reader->offset];
  reader->offset += sizeof(uint64_t);
  return ((uint64_t)bytes[0] << 0)
    | ((uint64_t)bytes[1] << 8)
    | ((uint64_t)bytes[2] << 16)
    | ((uint64_t)bytes[3] << 24)
    | ((uint64_t)bytes[4] << 32)
    | ((uint64_t)bytes[5] << 40)
    | ((uint64_t)bytes[6] << 48)
    | ((uint64_t)bytes[7] << 56);
}

static int checkpoint_read_i32(terminal_checkpoint_reader_t* reader) {
  return (int)(int32_t)checkpoint_read_u32(reader);
}

static const uint8_t* checkpoint_read_bytes(terminal_checkpoint_reader_t* reader,
    size_t length) {
  if (reader->failed || length > reader->size - reader->offset) {
    reader->failed = 1;
    return NULL;
  }
  const uint8_t* data = &reader->data[reader->offset];
  reader->offset += length;
  return data;
}

static size_t terminal_checkpoint_page_count(const terminal_t* terminal) {
  size_t count = 0;
  for (backbuffer_page_t* page = terminal->scrollback_buffer_end;
      page; page = page->next)
    ++count;
  return count;
}

static int terminal_checkpoint_view_size(const terminal_t* terminal,
    size_t* size) {
  for (int view_index = 0; view_index < VIEW_MAX; ++view_index) {
    if (!checkpoint_add_size(size, 15 * sizeof(uint32_t) + 2 * sizeof(uint64_t)))
      return 0;
    if (!checkpoint_add_size(size, 256 * sizeof(uint32_t)))
      return 0;
    size_t cells;
    if (!checkpoint_multiply_size(&cells, (size_t)terminal->columns,
        (size_t)terminal->lines))
      return 0;
    size_t overflow_size;
    size_t cell_size;
    if (!checkpoint_multiply_size(&overflow_size, (size_t)terminal->lines,
        sizeof(uint32_t))
        || !checkpoint_multiply_size(&cell_size, cells,
          sizeof(uint64_t) + sizeof(uint32_t)))
      return 0;
    if (!checkpoint_add_size(size, overflow_size))
      return 0;
    if (!checkpoint_add_size(size, cell_size))
      return 0;
  }
  return 1;
}

static int terminal_checkpoint_size_for(const terminal_t* terminal,
    size_t* size) {
  if (!terminal || terminal->closed || terminal->columns <= 0 || terminal->lines <= 0)
    return 0;
  if (terminal->scrollback_limit < 0)
    return 0;
  size_t cells;
  if (!checkpoint_multiply_size(&cells, (size_t)terminal->columns,
      (size_t)terminal->lines)
      || cells > TERMINAL_CHECKPOINT_MAX_CELLS)
    return 0;
  size_t page_count = terminal_checkpoint_page_count(terminal);
  if (page_count > UINT32_MAX || page_count > TERMINAL_CHECKPOINT_MAX_PAGES)
    return 0;
  if (!checkpoint_add_size(size, TERMINAL_CHECKPOINT_HEADER_SIZE))
    return 0;
  size_t name_length = terminal_bounded_string_length(terminal->name,
    sizeof(terminal->name));
  size_t buffered_length = terminal_bounded_string_length(
    terminal->buffered_sequence,
    sizeof(terminal->buffered_sequence));
  if (name_length >= sizeof(terminal->name)
      || buffered_length >= sizeof(terminal->buffered_sequence))
    return 0;
  if (!checkpoint_add_size(size, name_length)
      || !checkpoint_add_size(size, buffered_length)
      || !terminal_checkpoint_view_size(terminal, size))
    return 0;
  for (backbuffer_page_t* page = terminal->scrollback_buffer_end;
      page; page = page->next) {
    if (page->columns <= 0 || page->lines <= 0 || page->line < 0
        || page->line > page->lines)
      return 0;
    size_t page_cells;
    if (!checkpoint_multiply_size(&page_cells, (size_t)page->columns,
        (size_t)page->line)
        || page_cells > TERMINAL_CHECKPOINT_MAX_CELLS)
      return 0;
    size_t overflow_size;
    size_t cell_size;
    if (!checkpoint_multiply_size(&overflow_size, (size_t)page->line,
        sizeof(uint32_t))
        || !checkpoint_multiply_size(&cell_size, page_cells,
          sizeof(uint64_t) + sizeof(uint32_t)))
      return 0;
    if (!checkpoint_add_size(size, 3 * sizeof(uint32_t))
        || !checkpoint_add_size(size, overflow_size)
        || !checkpoint_add_size(size, cell_size))
      return 0;
  }
  return 1;
}

static void terminal_checkpoint_write_view(
    terminal_checkpoint_writer_t* writer, const terminal_t* terminal,
    const view_t* view) {
  checkpoint_write_i32(writer, view->cursor_x);
  checkpoint_write_i32(writer, view->cursor_y);
  checkpoint_write_i32(writer, view->cursor_styling_inversed);
  checkpoint_write_u64(writer, view->cursor_styling.value);
  checkpoint_write_i32(writer, view->saved_cursor_x);
  checkpoint_write_i32(writer, view->saved_cursor_y);
  checkpoint_write_i32(writer, view->saved_cursor_styling_inversed);
  checkpoint_write_u64(writer, view->saved_cursor_styling.value);
  checkpoint_write_i32(writer, view->saved_charset);
  checkpoint_write_i32(writer, view->cursor_mode);
  checkpoint_write_i32(writer, view->cursor_keys_mode);
  checkpoint_write_i32(writer, view->keypad_keys_mode);
  checkpoint_write_i32(writer, view->charset);
  checkpoint_write_i32(writer, view->last_graphical_character);
  checkpoint_write_i32(writer, view->tab_size);
  checkpoint_write_i32(writer, view->scrolling_region_start);
  checkpoint_write_i32(writer, view->scrolling_region_end);
  for (int i = 0; i < 256; ++i)
    checkpoint_write_u32(writer, view->palette[i].value);
  for (int row = 0; row < terminal->lines; ++row)
    checkpoint_write_i32(writer, view->overflows[row]);
  for (int row = 0; row < terminal->lines; ++row) {
    for (int column = 0; column < terminal->columns; ++column) {
      const buffer_char_t* cell = &view->buffer[row * terminal->columns + column];
      checkpoint_write_u64(writer, cell->styling.value);
      checkpoint_write_u32(writer, cell->codepoint);
    }
  }
}

static void terminal_checkpoint_write(
    terminal_checkpoint_writer_t* writer, const terminal_t* terminal,
    size_t size) {
  checkpoint_write_bytes(writer, terminal_checkpoint_magic,
    sizeof(terminal_checkpoint_magic));
  checkpoint_write_u32(writer, TERMINAL_CHECKPOINT_VERSION);
  checkpoint_write_u32(writer, (uint32_t)terminal->columns);
  checkpoint_write_u32(writer, (uint32_t)terminal->lines);
  checkpoint_write_u32(writer, (uint32_t)terminal->scrollback_limit);
  checkpoint_write_u32(writer, (uint32_t)terminal->current_view);
  checkpoint_write_u32(writer, (uint32_t)terminal->paste_mode);
  checkpoint_write_u32(writer, (uint32_t)terminal->mouse_tracking_mode);
  checkpoint_write_u32(writer, (uint32_t)terminal->mouse_encoding);
  checkpoint_write_u32(writer, (uint32_t)terminal->reporting_focus);
  checkpoint_write_u32(writer, (uint32_t)terminal->scrollback_position);
  size_t name_length = terminal_bounded_string_length(terminal->name,
    sizeof(terminal->name));
  size_t buffered_length = terminal_bounded_string_length(
    terminal->buffered_sequence,
    sizeof(terminal->buffered_sequence));
  checkpoint_write_u32(writer, (uint32_t)name_length);
  checkpoint_write_u32(writer, (uint32_t)buffered_length);
  checkpoint_write_u32(writer, (uint32_t)terminal_checkpoint_page_count(terminal));
  checkpoint_write_bytes(writer, terminal->name, name_length);
  checkpoint_write_bytes(writer, terminal->buffered_sequence, buffered_length);
  for (int i = 0; i < VIEW_MAX; ++i)
    terminal_checkpoint_write_view(writer, terminal, &terminal->views[i]);
  for (backbuffer_page_t* page = terminal->scrollback_buffer_end;
      page; page = page->next) {
    checkpoint_write_u32(writer, (uint32_t)page->columns);
    checkpoint_write_u32(writer, (uint32_t)page->lines);
    checkpoint_write_u32(writer, (uint32_t)page->line);
    int* overflows = (int*)&page->buffer[
      LIBTERMINAL_BACKBUFFER_PAGE_LINES * page->columns];
    for (int row = 0; row < page->line; ++row)
      checkpoint_write_i32(writer, overflows[row]);
    for (int row = 0; row < page->line; ++row) {
      for (int column = 0; column < page->columns; ++column) {
        const buffer_char_t* cell = &page->buffer[row * page->columns + column];
        checkpoint_write_u64(writer, cell->styling.value);
        checkpoint_write_u32(writer, cell->codepoint);
      }
    }
  }
  if (writer->offset != size)
    writer->failed = 1;
}

static int terminal_checkpoint_valid_view(const terminal_t* terminal,
    const view_t* view) {
  if (view->cursor_x < 0 || view->cursor_x >= terminal->columns
      || view->cursor_y < 0 || view->cursor_y >= terminal->lines
      || view->saved_cursor_x < 0 || view->saved_cursor_x >= terminal->columns
      || view->saved_cursor_y < 0 || view->saved_cursor_y >= terminal->lines
      || view->cursor_mode < CURSOR_SOLID || view->cursor_mode > CURSOR_BLINKING
      || view->saved_charset < CHARSET_US || view->saved_charset > CHARSET_OTHER
      || view->cursor_keys_mode < KEYS_MODE_NORMAL
      || view->cursor_keys_mode > KEYS_MODE_APPLICATION
      || view->keypad_keys_mode < KEYS_MODE_NORMAL
      || view->keypad_keys_mode > KEYS_MODE_APPLICATION
      || view->charset < CHARSET_US || view->charset > CHARSET_OTHER
      || view->scrolling_region_start < -1
      || view->scrolling_region_start >= terminal->lines
      || view->scrolling_region_end < -1
      || view->scrolling_region_end > terminal->lines
      || view->tab_size <= 0)
    return 0;
  return 1;
}

static int terminal_checkpoint_read_view(
    terminal_checkpoint_reader_t* reader, terminal_t* terminal, view_t* view) {
  view->cursor_x = checkpoint_read_i32(reader);
  view->cursor_y = checkpoint_read_i32(reader);
  view->cursor_styling_inversed = checkpoint_read_i32(reader);
  view->cursor_styling.value = checkpoint_read_u64(reader);
  view->saved_cursor_x = checkpoint_read_i32(reader);
  view->saved_cursor_y = checkpoint_read_i32(reader);
  view->saved_cursor_styling_inversed = checkpoint_read_i32(reader);
  view->saved_cursor_styling.value = checkpoint_read_u64(reader);
  view->saved_charset = (charset_e)checkpoint_read_i32(reader);
  view->cursor_mode = (cursor_mode_e)checkpoint_read_i32(reader);
  view->cursor_keys_mode = (keys_mode_e)checkpoint_read_i32(reader);
  view->keypad_keys_mode = (keys_mode_e)checkpoint_read_i32(reader);
  view->charset = (charset_e)checkpoint_read_i32(reader);
  view->last_graphical_character = checkpoint_read_i32(reader);
  view->tab_size = checkpoint_read_i32(reader);
  view->scrolling_region_start = checkpoint_read_i32(reader);
  view->scrolling_region_end = checkpoint_read_i32(reader);
  for (int i = 0; i < 256; ++i)
    view->palette[i].value = checkpoint_read_u32(reader);
  for (int row = 0; row < terminal->lines; ++row)
    view->overflows[row] = checkpoint_read_i32(reader);
  for (int row = 0; row < terminal->lines; ++row) {
    for (int column = 0; column < terminal->columns; ++column) {
      buffer_char_t* cell = &view->buffer[row * terminal->columns + column];
      cell->styling.value = checkpoint_read_u64(reader);
      cell->codepoint = checkpoint_read_u32(reader);
    }
  }
  return !reader->failed && terminal_checkpoint_valid_view(terminal, view);
}

static int terminal_emulator_restore_checkpoint_data(terminal_t* terminal,
    const void* data, size_t size) {
  if (!terminal || terminal->closed || !data || size < TERMINAL_CHECKPOINT_HEADER_SIZE)
    return 0;
  terminal_checkpoint_reader_t reader = {
    .data = (const uint8_t*)data, .size = size
  };
  const uint8_t* magic = checkpoint_read_bytes(&reader,
    sizeof(terminal_checkpoint_magic));
  if (reader.failed || memcmp(magic, terminal_checkpoint_magic,
      sizeof(terminal_checkpoint_magic)) != 0
      || checkpoint_read_u32(&reader) != TERMINAL_CHECKPOINT_VERSION)
    return 0;

  uint32_t columns = checkpoint_read_u32(&reader);
  uint32_t lines = checkpoint_read_u32(&reader);
  uint32_t scrollback_limit = checkpoint_read_u32(&reader);
  uint32_t current_view = checkpoint_read_u32(&reader);
  uint32_t paste_mode = checkpoint_read_u32(&reader);
  uint32_t mouse_tracking_mode = checkpoint_read_u32(&reader);
  uint32_t mouse_encoding = checkpoint_read_u32(&reader);
  uint32_t reporting_focus = checkpoint_read_u32(&reader);
  uint32_t scrollback_position = checkpoint_read_u32(&reader);
  uint32_t name_length = checkpoint_read_u32(&reader);
  uint32_t buffered_length = checkpoint_read_u32(&reader);
  uint32_t page_count = checkpoint_read_u32(&reader);
  if (reader.failed || !columns || !lines
      || columns > INT_MAX || lines > INT_MAX
      || scrollback_limit > INT_MAX || current_view >= VIEW_MAX
      || paste_mode > PASTE_BRACKETED
      || mouse_tracking_mode > MOUSE_TRACKING_ANY
      || mouse_encoding > MOUSE_ENCODING_SGR
      || name_length >= LIBTERMINAL_NAME_MAX
      || buffered_length >= LIBTERMINAL_CHUNK_SIZE
      || page_count > TERMINAL_CHECKPOINT_MAX_PAGES)
    return 0;
  size_t cells;
  if (!checkpoint_multiply_size(&cells, columns, lines)
      || cells > TERMINAL_CHECKPOINT_MAX_CELLS)
    return 0;

  terminal_t* restored = terminal_new((int)columns, (int)lines,
    (int)scrollback_limit);
  if (!restored)
    return 0;
  restored->current_view = (view_e)current_view;
  restored->paste_mode = (paste_mode_e)paste_mode;
  restored->mouse_tracking_mode = (mouse_tracking_mode_e)mouse_tracking_mode;
  restored->mouse_encoding = (mouse_encoding_e)mouse_encoding;
  restored->reporting_focus = (int)reporting_focus;
  const uint8_t* name = checkpoint_read_bytes(&reader, name_length);
  const uint8_t* buffered = checkpoint_read_bytes(&reader, buffered_length);
  if (reader.failed) {
    terminal_free(restored);
    return 0;
  }
  memcpy(restored->name, name, name_length);
  restored->name[name_length] = 0;
  memcpy(restored->buffered_sequence, buffered, buffered_length);
  restored->buffered_sequence[buffered_length] = 0;
  for (int i = 0; i < VIEW_MAX; ++i) {
    if (!terminal_checkpoint_read_view(&reader, restored, &restored->views[i])) {
      terminal_free(restored);
      return 0;
    }
  }

  backbuffer_page_t* last_page = NULL;
  for (uint32_t page_index = 0; page_index < page_count; ++page_index) {
    uint32_t page_columns = checkpoint_read_u32(&reader);
    uint32_t page_lines = checkpoint_read_u32(&reader);
    uint32_t page_line = checkpoint_read_u32(&reader);
    if (reader.failed || !page_columns || !page_lines
        || page_columns > INT_MAX
        || page_lines != LIBTERMINAL_BACKBUFFER_PAGE_LINES
        || page_line > page_lines)
      goto invalid_checkpoint;
    size_t page_cells;
    if (!checkpoint_multiply_size(&page_cells, page_columns, page_line)
        || page_cells > TERMINAL_CHECKPOINT_MAX_CELLS)
      goto invalid_checkpoint;
    size_t allocation = offsetof(backbuffer_page_t, buffer);
    if (!checkpoint_multiply_size(&page_cells, page_columns, page_lines)
        || page_cells > TERMINAL_CHECKPOINT_MAX_CELLS
        || !checkpoint_add_size(&allocation, page_cells * sizeof(buffer_char_t))
        || !checkpoint_add_size(&allocation, page_lines * sizeof(int)))
      goto invalid_checkpoint;
    backbuffer_page_t* page = calloc(1, allocation);
    if (!page)
      goto invalid_checkpoint;
    page->columns = (int)page_columns;
    page->lines = (int)page_lines;
    page->line = (int)page_line;
    page->prev = last_page;
    if (last_page)
      last_page->next = page;
    else
      restored->scrollback_buffer_end = page;
    last_page = page;
    restored->scrollback_buffer_start = last_page;
    int* overflows = (int*)&page->buffer[
      LIBTERMINAL_BACKBUFFER_PAGE_LINES * page->columns];
    for (uint32_t row = 0; row < page_line; ++row)
      overflows[row] = checkpoint_read_i32(&reader);
    for (uint32_t row = 0; row < page_line; ++row) {
      for (uint32_t column = 0; column < page_columns; ++column) {
        buffer_char_t* cell = &page->buffer[row * page->columns + column];
        cell->styling.value = checkpoint_read_u64(&reader);
        cell->codepoint = checkpoint_read_u32(&reader);
      }
    }
    if (reader.failed)
      goto invalid_checkpoint;
    if (restored->scrollback_total_lines > INT_MAX - (int)page_line)
      goto invalid_checkpoint;
    restored->scrollback_total_lines += (int)page_line;
  }
  restored->scrollback_buffer_start = last_page;
  restored->scrollback_position = 0;
  restored->scrollback_target = NULL;
  restored->scrollback_target_top_offset = 0;
  terminal_scrollback(restored, scrollback_position > INT_MAX
    ? INT_MAX : (int)scrollback_position);
  if (reader.failed || reader.offset != reader.size)
    goto invalid_checkpoint;

  terminal_input_callback_t input_callback = terminal->input_callback;
  void* input_callback_data = terminal->input_callback_data;
  int debug = terminal->debug;
  terminal_clear_scrollback_buffer(terminal);
  for (int i = 0; i < VIEW_MAX; ++i) {
    free(terminal->views[i].buffer);
    free(terminal->views[i].overflows);
  }
  *terminal = *restored;
  terminal->input_callback = input_callback;
  terminal->input_callback_data = input_callback_data;
  terminal->debug = debug;
  free(restored);
  return 1;

invalid_checkpoint:
  terminal_free(restored);
  return 0;
}


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
  return (int)(((const char*)up + 1) - p);
}

static int codepoint_to_utf8(unsigned int codepoint, char* target) {
  if (codepoint < 128) {
    *(target++) = (char)codepoint;
    return 1;
  } else if (codepoint < 2048) {
    *(target++) = (char)(0xC0 | (codepoint >> 6));
    *(target++) = (char)(0x80 | ((codepoint >> 0) & 0x3F));
    return 2;
  } else if (codepoint < 65536) {
    *(target++) = (char)(0xE0 | (codepoint >> 12));
    *(target++) = (char)(0x80 | ((codepoint >> 6) & 0x3F));
    *(target++) = (char)(0x80 | ((codepoint >> 0) & 0x3F));
    return 3;
  }
  *(target++) = (char)(0xF0 | (codepoint >> 18));
  *(target++) = (char)(0x80 | ((codepoint >> 12) & 0x3F));
  *(target++) = (char)(0x80 | ((codepoint >> 6) & 0x3F));
  *(target++) = (char)(0x80 | ((codepoint >> 0) & 0x3F));
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
  if (terminal->input_callback) {
    terminal->input_callback(str, len, terminal->input_callback_data);
    return 0;
  }
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
    backbuffer_page_t* page = calloc(
      offsetof(backbuffer_page_t, buffer)
        + LIBTERMINAL_BACKBUFFER_PAGE_LINES * terminal->columns * sizeof(buffer_char_t)
        + sizeof(int) * LIBTERMINAL_BACKBUFFER_PAGE_LINES,
      1
    );
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
      terminal->views[VIEW_ALTERNATE_BUFFER].palette[i] = indexed_color((uint8_t)i);
  }
}

static int parse_number(const char* seq, int def) {
  if (seq[0] >= '0' && seq[0] <= '9')
    return atoi(seq);
  return def;
}

static int parse_hex_byte(const char** cursor, uint8_t* value) {
  if (!isxdigit((unsigned char)**cursor))
    return 0;
  errno = 0;
  char* end;
  unsigned long parsed = strtoul(*cursor, &end, 16);
  if (errno == ERANGE || end == *cursor || parsed > UINT8_MAX)
    return 0;
  *value = (uint8_t)parsed;
  *cursor = end;
  return 1;
}

static int parse_osc_rgb(const char* text, int* index, uint8_t* r, uint8_t* g, uint8_t* b) {
  const char* cursor = text;
  if (*cursor++ != ';')
    return 0;

  errno = 0;
  char* end;
  long parsed_index = strtol(cursor, &end, 10);
  if (errno == ERANGE || end == cursor || parsed_index < 0 || parsed_index > 255 || *end != ';')
    return 0;
  ++end;
  *index = (int)parsed_index;

  if (strncmp(end, "rgb:", 4) != 0)
    return 0;
  cursor = end + 4;
  if (!parse_hex_byte(&cursor, r) || *cursor++ != '/')
    return 0;
  if (!parse_hex_byte(&cursor, g) || *cursor++ != '/')
    return 0;
  if (!parse_hex_byte(&cursor, b) || *cursor != '\0')
    return 0;
  return 1;
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
    int seq_end = (int)strlen(seq) - 1;
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
        terminal_input(terminal, "\033[?1;2c", 7);
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
            const char* separator = strstr(next, ";");
            next = separator ? separator + 1 : NULL;
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
            const char* separator = strstr(next, ";");
            next = separator ? separator + 1 : NULL;
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
          offset = (int)(next - seq) + 1;
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
        if (strlen(seq) >= 5 && seq[3] == ';') {
          size_t name_length = strlen(&seq[4]);
          if (name_length >= sizeof(terminal->name))
            name_length = sizeof(terminal->name) - 1;
          memcpy(terminal->name, &seq[4], name_length);
          terminal->name[name_length] = '\0';
        }
      break;
      case '4': {
        int idx;
        uint8_t r, g, b;
        if (parse_osc_rgb(&seq[3], &idx, &r, &g, &b)) {
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
        int region_end = (view->scrolling_region_end == -1 ? terminal->lines : min(view->scrolling_region_end, terminal->lines));
        if (view->cursor_y == region_end - 1)
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
          int region_end = min(view->scrolling_region_end, terminal->lines);
          if (view->cursor_y == start) {
            terminal_scroll_region_down(terminal, start, region_end, 1);
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
  int buffered_sequence_index = (int)strlen(terminal->buffered_sequence);
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
        (escape_type == ESCAPE_TYPE_OS &&
          ((offset < len - 1 && str[offset+1] == '\a') ||
           (offset < len - 2 && str[offset+1] == 0x1B && str[offset+2] == 0x5C))) ||
        (escape_type == ESCAPE_TYPE_UNKNOWN && str[offset] == 0x1B) ||
        ((escape_type == ESCAPE_TYPE_FIXED_WIDTH && buffered_sequence_index == fixed_width) ||
          str[offset] == 0x1B)
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
      if (codepoint != 0x1B)
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

static void terminal_resize(terminal_t* terminal, int columns, int lines) {
  if (terminal->columns == columns && terminal->lines == lines)
    return;
  for (int i = 0; i < VIEW_MAX; ++i) {
    buffer_char_t* buffer = malloc(sizeof(buffer_char_t) * columns * lines);
    int* overflows = calloc(lines * sizeof(int), 1);
    if (!buffer || !overflows) {
      free(buffer);
      free(overflows);
      continue;
    }
    memset(buffer, 0, sizeof(buffer_char_t) * columns * lines);
    if (terminal->views[i].buffer) {
      if (lines < terminal->lines && i == VIEW_NORMAL_BUFFER) {
        for (int j = 0; j < max(0, (terminal->views[i].cursor_y + 1) - lines); ++j)
          terminal_shift_buffer(terminal);
      }
      int max_lines = min(terminal->lines, lines);
      for (int y = 0; y < max_lines; ++y)
        memcpy(&buffer[y * columns], &terminal->views[i].buffer[y * terminal->columns],
          min(terminal->columns, columns) * sizeof(buffer_char_t));
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
    if (terminal->views[i].scrolling_region_end != -1
        || terminal->views[i].scrolling_region_start != -1) {
      terminal->views[i].scrolling_region_start = min(
        terminal->views[i].scrolling_region_start, lines - 1);
      terminal->views[i].scrolling_region_end = min(
        terminal->views[i].scrolling_region_end, lines);
    }
    terminal->views[i].overflows = overflows;
  }
  terminal->columns = columns;
  terminal->lines = lines;
}

static terminal_t* terminal_new(int columns, int lines, int scrollback_limit) {
  terminal_t* terminal = calloc(1, sizeof(*terminal));
  if (!terminal)
    return NULL;
  for (int i = 0; i < VIEW_MAX; ++i) {
    for (int j = 0; j < 256; ++j)
      terminal->views[i].palette[j] = indexed_color((uint8_t)j);
    terminal->views[i].scrolling_region_end = -1;
    terminal->views[i].scrolling_region_start = -1;
    terminal->views[i].cursor_styling = LIBTERMINAL_NO_STYLING;
    terminal->views[i].saved_cursor_styling = LIBTERMINAL_NO_STYLING;
    terminal->views[i].saved_charset = CHARSET_US;
    terminal->views[i].tab_size = LIBTERMINAL_DEFAULT_TAB_SIZE;
  }
  terminal->scrollback_limit = scrollback_limit;
  terminal_resize(terminal, columns, lines);
  return terminal;
}

static int terminal_close(terminal_t* terminal) {
  if (!terminal || terminal->closed)
    return 0;
  terminal->closed = 1;
  terminal_clear_scrollback_buffer(terminal);
  for (int i = 0; i < VIEW_MAX; ++i) {
    free(terminal->views[i].buffer);
    free(terminal->views[i].overflows);
    terminal->views[i].buffer = NULL;
    terminal->views[i].overflows = NULL;
  }
  return 0;
}

static void terminal_free(terminal_t* terminal) {
  if (!terminal)
    return;
  terminal_close(terminal);
  free(terminal);
}

static int terminal_emit_line(terminal_emulator_line_callback callback,
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

terminal_emulator_t* terminal_emulator_new(
    int columns, int rows, int scrollback_limit, const char* term) {
  (void)term;
  return (terminal_emulator_t*)terminal_new(columns, rows, scrollback_limit);
}

void terminal_emulator_free(terminal_emulator_t* emulator) {
  terminal_free((terminal_t*)emulator);
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
  terminal_t* terminal = (terminal_t*)emulator;
  return terminal ? terminal_output(terminal, data, (int)length) : 0;
}

size_t terminal_emulator_checkpoint_size(terminal_emulator_t* emulator) {
  size_t size = 0;
  return terminal_checkpoint_size_for((terminal_t*)emulator, &size) ? size : 0;
}

int terminal_emulator_checkpoint(terminal_emulator_t* emulator,
    void* data, size_t size, size_t* written) {
  terminal_t* terminal = (terminal_t*)emulator;
  size_t required = 0;
  if (!terminal_checkpoint_size_for(terminal, &required))
    return 0;
  if (written)
    *written = required;
  if (!data || size < required)
    return 0;
  terminal_checkpoint_writer_t writer = {
    .data = (uint8_t*)data, .size = size
  };
  terminal_checkpoint_write(&writer, terminal, required);
  return !writer.failed;
}

int terminal_emulator_restore_checkpoint(terminal_emulator_t* emulator,
    const void* data, size_t size) {
  return terminal_emulator_restore_checkpoint_data((terminal_t*)emulator,
    data, size);
}

void terminal_emulator_resize(terminal_emulator_t* emulator,
    int columns, int rows) {
  terminal_t* terminal = (terminal_t*)emulator;
  if (terminal && !terminal->closed)
    terminal_resize(terminal, columns, rows);
}

int terminal_emulator_close(terminal_emulator_t* emulator) {
  return terminal_close((terminal_t*)emulator);
}

int terminal_emulator_is_closed(terminal_emulator_t* emulator) {
  terminal_t* terminal = (terminal_t*)emulator;
  return !terminal || terminal->closed;
}

void terminal_emulator_clear_scrollback(terminal_emulator_t* emulator) {
  terminal_t* terminal = (terminal_t*)emulator;
  if (terminal)
    terminal_clear_scrollback_buffer(terminal);
}

void terminal_emulator_clear(terminal_emulator_t* emulator) {
  terminal_t* terminal = (terminal_t*)emulator;
  if (!terminal || terminal->closed)
    return;
  terminal_clear_scrollback_buffer(terminal);
  view_t* view = &terminal->views[terminal->current_view];
  memset(view->buffer, 0, sizeof(buffer_char_t) * terminal->columns * terminal->lines);
  memset(view->overflows, 0, sizeof(int) * terminal->lines);
  view->cursor_x = 0;
  view->cursor_y = 0;
}

void terminal_emulator_reset(terminal_emulator_t* emulator) {
  terminal_t* terminal = (terminal_t*)emulator;
  if (!terminal || terminal->closed)
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
    memset(view->buffer, 0, sizeof(buffer_char_t) * terminal->columns * terminal->lines);
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
      view->palette[color] = indexed_color((uint8_t)color);
  }
}

void terminal_emulator_dimensions(terminal_emulator_t* emulator,
    int* columns, int* rows) {
  terminal_t* terminal = (terminal_t*)emulator;
  if (!terminal)
    return;
  if (columns) *columns = terminal->columns;
  if (rows) *rows = terminal->lines;
}

int terminal_emulator_cursor(terminal_emulator_t* emulator,
    int* column, int* row, int* mode) {
  terminal_t* terminal = (terminal_t*)emulator;
  if (!terminal)
    return 0;
  view_t* view = &terminal->views[terminal->current_view];
  if (column) *column = view->cursor_x;
  if (row) *row = view->cursor_y;
  if (mode) *mode = view->cursor_mode;
  return 1;
}

void terminal_emulator_modes(terminal_emulator_t* emulator,
    int* cursor_keys_mode, int* keypad_keys_mode, int* mouse_tracking_mode,
    int* mouse_encoding, int* paste_mode, int* reporting_focus) {
  terminal_t* terminal = (terminal_t*)emulator;
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

const char* terminal_emulator_name(terminal_emulator_t* emulator) {
  terminal_t* terminal = (terminal_t*)emulator;
  if (!terminal || !terminal->name[0])
    return NULL;
  return terminal->name;
}

void terminal_emulator_focus(terminal_emulator_t* emulator, int focused) {
  terminal_input((terminal_t*)emulator, focused ? "\x1B[" : "\x1B[O", 3);
}

void terminal_emulator_scrollback(terminal_emulator_t* emulator,
    int position, int* current, int* total) {
  terminal_t* terminal = (terminal_t*)emulator;
  if (!terminal)
    return;
  if (position >= 0)
    terminal_scrollback(terminal, position);
  if (current) *current = terminal->scrollback_position;
  if (total) *total = terminal->scrollback_total_lines;
}

int terminal_emulator_for_each_line(terminal_emulator_t* emulator,
    int first_row, int last_row, terminal_emulator_line_callback callback,
    void* user_data) {
  terminal_t* terminal = (terminal_t*)emulator;
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
        terminal_emit_line(callback, user_data, first_row + emitted,
          &page->buffer[row * page->columns], page->columns, overflows[row]);
        ++emitted;
      }
      page = page->next;
      lines_into_page = 0;
    }
  }
  int active_row = max(first_row, 0);
  while (active_row < terminal->lines && emitted < requested) {
    terminal_emit_line(callback, user_data, first_row + emitted,
      &view->buffer[active_row * terminal->columns], terminal->columns,
      view->overflows[active_row]);
    ++active_row;
    ++emitted;
  }
  return emitted;
}
