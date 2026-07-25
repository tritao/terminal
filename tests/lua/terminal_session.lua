local test = require "core.test"

local available, terminal = pcall(require, "plugins.terminal")

test.describe("terminal session boundary", function()
  test.test("exposes the backend-independent session contract", function()
    test.skip_if(not available, "terminal plugin is unavailable: " .. tostring(terminal))
    local written, resized, detached = {}, nil, false
    local session = terminal.Session {
      id = "fake-session",
      status = "starting",
      write = function(_, data) written[#written + 1] = data end,
      resize = function(_, columns, rows) resized = { columns, rows } end,
      detach = function() detached = true end,
      poll_events = function()
        return {
          { type = "output", runtime_id = "fake-session", offset = 0, data = "hello" },
          { type = "status", runtime_id = "fake-session", status = "running" }
        }
      end
    }
    test.equal(session:id(), "fake-session")
    test.equal(session:status(), "starting")
    session:write("input")
    session:resize(80, 24)
    local events = session:poll_events()
    session:detach()
    test.equal(written[1], "input")
    test.same(resized, { 80, 24 })
    test.equal(#events, 2)
    test.equal(session:status(), "running")
    test.ok(detached)
  end)

  test.test("normalizes local launch specifications and reports capabilities", function()
    test.skip_if(not available, "terminal plugin is unavailable: " .. tostring(terminal))
    local spec = terminal.normalize_launch_spec { command = "/bin/sh", cwd = "/tmp" }
    test.equal(spec.command, "/bin/sh")
    test.equal(spec.cwd, "/tmp")
    test.type(spec.args, "table")
    terminal.register_profile("terminal-session-test", {
      command = "/bin/sh", args = { "-l" },
      environment = { PROFILE_VALUE = "profile" }
    })
    local profile = terminal.normalize_launch_spec {
      profile = "terminal-session-test", environment = { OVERRIDE = "ok" }
    }
    test.equal(profile.args[1], "-l")
    test.equal(profile.environment.PROFILE_VALUE, "profile")
    test.equal(profile.environment.OVERRIDE, "ok")
    test.ok(terminal.supported_capabilities().local_backend)
    test.ok(terminal.supported_capabilities().sessions)
  end)

  test.test("feeds a standalone emulator without a process", function()
    test.skip_if(not available, "terminal plugin is unavailable: " .. tostring(terminal))
    local emulator = terminal.new_emulator { columns = 20, rows = 4, scrollback = 8 }
    local shifts = emulator:feed("hello\r\n")
    test.type(shifts, "number")
    local found = false
    for _, line in ipairs(emulator:lines()) do
      for i = 2, #line, 2 do
        if line[i]:find("hello", 1, true) then found = true end
      end
    end
    test.ok(found, "terminal emulator did not render fed text")
    emulator:reset()
    local cursor_x, cursor_y = emulator:cursor()
    test.equal(cursor_x, 0)
    test.equal(cursor_y, 0)
    emulator:close()
  end)

  test.test("opens a supplied session as a regular view", function()
    test.skip_if(not available, "terminal plugin is unavailable: " .. tostring(terminal))
    local detached = false
    local emulator = terminal.new_emulator { columns = 20, rows = 4 }
    local session = terminal.Session {
      id = "view-session", emulator = emulator,
      write = function() end, resize = function() end,
      detach = function() detached = true end,
      poll_events = function() return {} end
    }
    local view = terminal.open_session(session, { activate = false })
    test.ok(view:extends(terminal.class))
    test.equal(view.session, session)
    test.not_nil(core.root_view.root_node:get_node_for_view(view))
    view:close()
    test.ok(detached)
    emulator:close()
  end)
end)
