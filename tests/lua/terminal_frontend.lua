local core = require "core"
local command = require "core.command"
local common = require "core.common"
local keymap = require "core.keymap"
local test = require "core.test"
local renderer = require "renderer"

local available, terminal = pcall(require, "plugins.terminal")

test.describe("terminal frontend", function()
  local function new_view(text, options)
    options = options or {}
    local emulator = terminal.new_emulator {
      columns = options.columns or 40,
      rows = options.rows or 3,
      scrollback = options.scrollback or 16
    }
    emulator:feed(text)
    return terminal.class { emulator = emulator }, emulator
  end

  test.test("routes special keys to derived terminal views", function()
    test.skip_if(not available, "terminal plugin is unavailable: " .. tostring(terminal))
    local DerivedTerminalView = terminal.class:extend()
    local emulator = terminal.new_emulator { columns = 40, rows = 3, scrollback = 16 }
    local received
    local session = terminal.Session {
      id = "derived-terminal-input",
      emulator = emulator,
      write = function(_, data)
        received = data
        return true
      end,
      resize = function() return true end,
      poll_events = function() return {} end,
      detach = function() return true end
    }
    local view = DerivedTerminalView { session = session, emulator = emulator }
    local node = core.root_view:get_primary_node()
    node:add_view(view)
    core.set_active_view(view)

    test.ok(keymap.on_key_pressed("backspace"))
    test.equal(received, "\x7F")

    emulator:feed("\27[>7u")
    test.ok(keymap.on_key_pressed("tab"))
    test.equal(received, "\t")

    view:close()
    emulator:close()
  end)

  test.test("falls back when a terminal palette entry is missing", function()
    test.skip_if(not available, "terminal plugin is unavailable: " .. tostring(terminal))
    local fallback = { 12, 34, 56, 255 }
    local view = terminal.class {
      text = fallback,
      colors = {}
    }
    view.options.colors[12] = nil

    local color, attributes = view:convert_color(2 * 0x1000000 + 12 * 0x10000, "foreground")
    test.equal(color, fallback)
    test.equal(attributes, 2)

    color = view:convert_color(2 * 0x1000000, "foreground")
    test.equal(color, view.options.colors[0])

    view.options.colors[12] = { 0, 0 }
    color = view:convert_color(2 * 0x1000000 + 12 * 0x10000, "foreground")
    test.equal(color, fallback)

    color, attributes = view:convert_color(4 * 0x1000000, "foreground")
    test.equal(color, fallback)
    test.equal(attributes, 4)
  end)

  test.test("publishes only completed synchronized-output frames", function()
    test.skip_if(not available, "terminal plugin is unavailable: " .. tostring(terminal))
    local emulator = terminal.new_emulator { columns = 12, rows = 1 }
    local view = terminal.class { emulator = emulator }

    local function publish() view:capture_render_snapshot() end
    emulator:feed_frames("\27[?2026h\27[1;1Happs\27[?20", publish)
    test.equal(view.render_snapshot, nil)
    emulator:feed_frames("26l\27[?2026h\27[1;1Hxxxx", publish)

    local rendered = {}
    for index = 2, #view.render_snapshot.lines[1], 2 do
      rendered[#rendered + 1] = view.render_snapshot.lines[1][index]
    end
    test.contains(table.concat(rendered), "apps")

    local current = {}
    for index = 2, #emulator:lines()[1], 2 do
      current[#current + 1] = emulator:lines()[1][index]
    end
    test.contains(table.concat(current), "xxxx")
    emulator:close()
  end)

  test.test("replays Codex startup progress frames without shifting cells", function()
    test.skip_if(not available, "terminal plugin is unavailable: " .. tostring(terminal))
    local emulator = terminal.new_emulator { columns = 96, rows = 24 }
    local expected = "• Starting MCP servers (1/3): codex_apps, openaiDeveloperDocs (0s • esc to interrupt)"
    local frames = 0

    local function line_text(line)
      local result = {}
      for index = 2, #line, 2 do result[#result + 1] = line[index] end
      return table.concat(result):gsub("\n$", "")
    end

    local function verify_frame()
      frames = frames + 1
      local line = emulator:lines()[19]
      test.equal(line_text(line):sub(1, #expected), expected)

      -- Codex's shimmer splits this row into many differently-colored runs.
      -- Every run must retain its native cell column; deriving columns from
      -- UTF-8 byte or codepoint counts is what caused the original drift.
      local previous_end = 0
      for _, run in ipairs(emulator:cell_lines()[19]) do
        local text, column, cells = run[2], run[3], run[4]
        test.ok(column >= previous_end, "Codex progress runs overlap")
        if text:match("^[%z\1-\127]*$") then
          test.equal(cells, #text)
        end
        previous_end = column + cells
      end
    end

    local base = "\27[?2026h\27[19;1H\27[1m• Starting MCP servers (1/3): codex_apps, openaiDeveloperDocs\27[22m \27[2m(0s • esc to interrupt)\27[0m\27[?2026l"
    local shimmer = table.concat {
      "\27[?2026h\27[19;3H\27[1m\27[38;2;209;209;209mS\27[0m\27[?2026l",
      "\27[?2026h\27[19;3H\27[1m\27[38;2;157;157;157mS\27[38;2;209;209;209mt\27[0m\27[?2026l",
      "\27[?2026h\27[19;3H\27[1m\27[38;2;42;42;42mS\27[38;2;94;94;94mt\27[38;2;157;157;157ma\27[38;2;209;209;209mr\27[0m\27[?2026l",
      "\27[?2026h\27[19;3H\27[1m\27[38;2;22;22;22mS\27[38;2;42;42;42mt\27[38;2;94;94;94ma\27[38;2;157;157;157mr\27[38;2;209;209;209mt\27[0m\27[?2026l"
    }

    -- Deliberately split the synchronized-output terminator as a backend can.
    emulator:feed_frames(base:sub(1, -4), verify_frame)
    test.equal(frames, 0)
    emulator:feed_frames(base:sub(-3) .. shimmer, verify_frame)
    test.equal(frames, 5)

    local view = terminal.class { emulator = emulator }
    view.position.x, view.position.y = 17, 11
    view:capture_render_snapshot()
    local target_y = view.position.y + view.options.padding.y
      + 18 * view.options.font:get_height()
    local calls = {}
    local background_calls = 0
    local original_draw_text = renderer.draw_text
    local original_draw_rect = renderer.draw_rect
    local original_draw_background = terminal.class.super.draw_background
    local original_draw_scrollbar = terminal.class.super.draw_scrollbar
    terminal.class.super.draw_background = function() end
    terminal.class.super.draw_scrollbar = function() end
    renderer.draw_rect = function(_, y)
      if y == target_y then background_calls = background_calls + 1 end
    end
    renderer.draw_text = function(font, text, x, y, color, tab_data)
      if y == target_y then
        calls[#calls + 1] = { text = text, x = x }
      end
      return x + font:get_width(text, tab_data)
    end
    local drew, draw_error = pcall(function() view:draw() end)
    renderer.draw_text = original_draw_text
    renderer.draw_rect = original_draw_rect
    terminal.class.super.draw_background = original_draw_background
    terminal.class.super.draw_scrollbar = original_draw_scrollbar
    test.ok(drew, draw_error)

    local runs = emulator:cell_lines()[19]
    local cell_width = view.options.font:get_width("W")
    test.equal(background_calls, 0)
    test.equal(#calls, #runs)
    for index, run in ipairs(runs) do
      test.equal(calls[index].text, run[2])
      test.equal(calls[index].x,
        view.position.x + view.options.padding.x + run[3] * cell_width)
    end
    emulator:close()
  end)

  test.test("preserves native cell geometry for wide glyphs", function()
    test.skip_if(not available, "terminal plugin is unavailable: " .. tostring(terminal))
    local emulator = terminal.new_emulator { columns = 12, rows = 1 }
    emulator:feed("A界B\27[31mX")
    local runs = emulator:cell_lines()[1]
    test.equal(runs[1][3], 0)
    test.equal(runs[1][4], 3)
    test.contains(runs[1][2], "A界")
    local red_run
    for _, run in ipairs(runs) do
      if run[2]:find("X", 1, true) then red_run = run break end
    end
    test.equal(red_run[3], 4)
    emulator:close()

    emulator = terminal.new_emulator { columns = 30, rows = 1 }
    emulator:feed("│ ✨ Update │\27[31mX")
    runs = emulator:cell_lines()[1]
    for _, run in ipairs(runs) do
      if run[2]:find("X", 1, true) then red_run = run break end
    end
    test.equal(red_run[3], 13)
    emulator:close()
  end)

  test.test("searches visible output and scrollback with case options", function()
    test.skip_if(not available, "terminal plugin is unavailable: " .. tostring(terminal))
    local view, emulator = new_view("old TARGET\r\nnew target\r\nvisible target", { rows = 2 })

    local matches = view:find_matches("target", false)
    test.equal(#matches, 3)
    test.ok(matches[1].row < 0, "the first match should be in scrollback")
    test.equal(#view:find_matches("target", true), 2)

    view:set_search_text("target")
    test.equal(view.search_state.index, 1)
    local next_match = view:search_next(false)
    test.equal(next_match.text, "target")
    test.equal(view.search_state.index, 2)
    local previous_match = view:search_next(true)
    test.equal(previous_match.row, matches[1].row)

    view:toggle_search_case_sensitive()
    test.equal(#view.search_state.matches, 2)
    emulator:close()
  end)

  test.test("searches correctly while already scrolled back", function()
    test.skip_if(not available, "terminal plugin is unavailable: " .. tostring(terminal))
    local view, emulator = new_view(
      "old target\r\nmiddle line\r\nvisible target", { rows = 2 }
    )
    local cached_lines = view:get_output_lines()
    test.equal(view:get_output_lines(), cached_lines)
    local _, total_scrollback = emulator:scrollback()
    test.ok(total_scrollback > 0)
    emulator:scrollback(total_scrollback)
    test.not_equal(view:get_output_lines(), cached_lines)

    local matches = view:find_matches("target", false)
    test.equal(#matches, 2)
    view:set_search_text("visible")
    test.equal(view.search_state.match.text, "visible")
    test.equal(emulator:scrollback(), 0)
    emulator:close()
  end)

  test.test("detects URLs in terminal text and trims punctuation", function()
    test.skip_if(not available, "terminal plugin is unavailable: " .. tostring(terminal))
    local view, emulator = new_view(
      "see https://example.com/path, or http://localhost:8080/x!\r\n",
      { columns = 80 }
    )
    local links = view:get_links()
    test.equal(#links, 2)
    test.equal(view:get_links(), links)
    test.equal(links[1].url, "https://example.com/path")
    test.equal(links[2].url, "http://localhost:8080/x")
    emulator:close()
  end)

  test.test("invalidates cached links when session output arrives", function()
    test.skip_if(not available, "terminal plugin is unavailable: " .. tostring(terminal))
    local emulator = terminal.new_emulator { columns = 40, rows = 3, scrollback = 16 }
    local pending = true
    local session = terminal.Session {
      id = "frontend-cache-session",
      emulator = emulator,
      write = function() end,
      resize = function() end,
      poll_events = function()
        if not pending then return {} end
        pending = false
        return {
          { type = "output", runtime_id = "frontend-cache-session",
            offset = 0, data = "https://new.example" }
        }
      end
    }
    local view = terminal.class { session = session, emulator = emulator }
    test.equal(#view:get_links(), 0)
    view:shift_selection_update()
    test.equal(view:get_links()[1].url, "https://new.example")
    view:close()
    emulator:close()
  end)

  test.test("clears a hovered link when the mouse leaves", function()
    test.skip_if(not available, "terminal plugin is unavailable: " .. tostring(terminal))
    local view, emulator = new_view("https://example.com")
    view.convert_coordinates = function() return 2, 0, 2 end
    view:on_mouse_moved(100, 100, 0, 0)
    test.equal(view.hovered_link_url, "https://example.com")
    view:on_mouse_left()
    test.is_nil(view.hovered_link_url)
    test.equal(view.cursor, "ibeam")
    emulator:close()
  end)

  test.test("registers terminal link context-menu actions", function()
    test.skip_if(not available, "terminal plugin is unavailable: " .. tostring(terminal))
    local contextmenu_available, contextmenu = pcall(require, "plugins.contextmenu")
    test.skip_if(not contextmenu_available, "context menu plugin is unavailable: " .. tostring(contextmenu))
    local view, emulator = new_view("https://example.com")
    local node = core.root_view:get_primary_node()
    node:add_view(view)
    core.set_active_view(view)
    view.convert_coordinates = function() return 2, 0, 2 end

    test.ok(contextmenu:show(0, 0))
    local items = {}
    for _, item in ipairs(contextmenu.items) do
      if item.text then items[item.text] = true end
    end
    test.ok(items["Open Link"])
    test.ok(items["Copy Link"])
    contextmenu:hide()
    view:close()
    emulator:close()
  end)

  test.test("keeps terminal selection coordinates and text extraction", function()
    test.skip_if(not available, "terminal plugin is unavailable: " .. tostring(terminal))
    local view, emulator = new_view("select me\r\n")
    view.selection = { 0, 0, 6, 0 }
    test.equal(view:get_text(0, 0, 0, 6), "select")
    test.same(view:sorted_selection(), { 0, 0, 6, 0 })
    emulator:close()
  end)

  test.test("activates the URL under the mouse", function()
    test.skip_if(not available, "terminal plugin is unavailable: " .. tostring(terminal))
    local view, emulator = new_view("https://example.com")
    local opened
    local original_open_in_system = common.open_in_system
    common.open_in_system = function(url) opened = url end
    view.convert_coordinates = function() return 2, 0, 2 end
    local link = view:open_link_at(0, 0)
    common.open_in_system = original_open_in_system
    test.equal(link.url, "https://example.com")
    test.equal(opened, "https://example.com")
    emulator:close()
  end)

  test.test("registers Terminal: Search through the command overlay", function()
    test.skip_if(not available, "terminal plugin is unavailable: " .. tostring(terminal))
    local view, emulator = new_view("searchable output")
    local node = core.root_view:get_primary_node()
    node:add_view(view)
    core.set_active_view(view)

    test.ok(command.perform("terminal:search"))
    test.equal(core.active_view, core.command_view)
    test.equal(core.command_view.label, "Search Terminal: ")
    core.command_view:exit(false)
    view:close()
    emulator:close()
  end)
end)
