local core = require "core"
local command = require "core.command"
local common = require "core.common"
local keymap = require "core.keymap"
local test = require "core.test"

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

    view:close()
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
