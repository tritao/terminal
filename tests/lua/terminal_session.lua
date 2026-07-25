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

  test.test("tracks output offsets and requests replay for gaps", function()
    test.skip_if(not available, "terminal plugin is unavailable: " .. tostring(terminal))
    local replay_offsets = {}
    local pending = {
      { type = "output", runtime_id = "offset-session", offset = 0, data = "abc" },
      { type = "output", runtime_id = "offset-session", offset = 0, data = "abc" },
      { type = "output", runtime_id = "offset-session", offset = 7, data = "later" },
      { type = "status", runtime_id = "offset-session", status = "running" }
    }
    local session = terminal.Session {
      id = "offset-session",
      capabilities = { replay = true },
      write = function() end,
      resize = function() end,
      request_replay = function(_, offset) replay_offsets[#replay_offsets + 1] = offset end,
      poll_events = function()
        local events = pending
        pending = {}
        return events
      end
    }

    local events = session:poll_events()
    test.equal(session:offset(), 3)
    test.equal(session.duplicate_events, 1)
    test.equal(session.gap_events, 1)
    test.same(replay_offsets, { 3 })
    test.equal(#events, 3)
    test.equal(events[1].type, "output")
    test.equal(events[2].type, "gap")
    test.equal(events[3].type, "status")
    test.equal(events[2].offset, 3)
    test.equal(events[2].received_offset, 7)
  end)

  test.test("attaches once and applies a checkpoint before live output", function()
    test.skip_if(not available, "terminal plugin is unavailable: " .. tostring(terminal))
    local attached, detached = 0, 0
    local replay_offsets = {}
    local source = terminal.new_emulator { columns = 20, rows = 4 }
    source:feed("checkpoint\r\n")
    local checkpoint_data = source:checkpoint()
    local emulator = terminal.new_emulator { columns = 5, rows = 2 }
    local pending = {
      { type = "checkpoint", runtime_id = "remote-session", offset = 0,
        data = checkpoint_data },
      { type = "output", runtime_id = "remote-session", offset = 0, data = "live" }
    }
    local session = terminal.Session {
      id = "remote-session",
      emulator = emulator,
      capabilities = { replay = true, events_applied = false },
      attach = function() attached = attached + 1 end,
      write = function() end,
      resize = function() end,
      request_replay = function(_, offset) replay_offsets[#replay_offsets + 1] = offset end,
      detach = function() detached = detached + 1 end,
      poll_events = function()
        local event = table.remove(pending, 1)
        return event and { event } or {}
      end
    }

    local view = terminal.open_session(session, {
      activate = false, node = core.root_view:get_primary_node()
    })
    view:shift_selection_update()
    view:shift_selection_update()
    test.equal(attached, 1)
    test.equal(session:offset(), 4)
    test.same(replay_offsets, { 0 })
    test.ok(view.terminal == emulator)

    local found_live = false
    for _, line in ipairs(view.terminal:lines()) do
      for i = 2, #line, 2 do
        if line[i]:find("live", 1, true) then found_live = true end
      end
    end
    test.ok(found_live, "checkpoint emulator did not receive live output")
    view:close()
    test.equal(detached, 1)
    source:close()
    emulator:close()
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
    local view = terminal.open_session(session, {
      activate = false, node = core.root_view:get_primary_node()
    })
    test.ok(view:extends(terminal.class))
    test.equal(view.session, session)
    test.not_nil(core.root_view.root_node:get_node_for_view(view))
    view:close()
    test.ok(detached)
    emulator:close()
  end)
end)
