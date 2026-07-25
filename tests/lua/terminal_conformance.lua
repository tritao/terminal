local test = require "core.test"

local available, terminal = pcall(require, "plugins.terminal")

local function contains_terminal_text(emulator, text)
  for _, line in ipairs(emulator:lines()) do
    for i = 2, #line, 2 do
      if line[i]:find(text, 1, true) then return true end
    end
  end
  return false
end

test.describe("terminal backend conformance", function()
  test.test("renders a remote session and forwards backend operations", function()
    test.skip_if(not available, "terminal plugin is unavailable: " .. tostring(terminal))
    local calls = { attach = 0, detach = 0, replay = {}, writes = {}, resizes = {} }
    local pending = {
      { type = "output", runtime_id = "remote-conformance", offset = 0,
        data = "remote session\r\n" },
      { type = "status", runtime_id = "remote-conformance", status = "running" }
    }
    local emulator = terminal.new_emulator { columns = 20, rows = 4 }
    local session = terminal.Session {
      id = "remote-conformance",
      emulator = emulator,
      capabilities = { replay = true, events_applied = false },
      attach = function() calls.attach = calls.attach + 1 end,
      write = function(_, data) calls.writes[#calls.writes + 1] = data end,
      resize = function(_, columns, rows)
        calls.resizes[#calls.resizes + 1] = { columns, rows }
      end,
      request_replay = function(_, offset) calls.replay[#calls.replay + 1] = offset end,
      detach = function() calls.detach = calls.detach + 1 end,
      poll_events = function()
        local events = pending
        pending = {}
        return events
      end
    }

    local view = terminal.class { session = session, emulator = emulator }
    view:shift_selection_update()
    session:write("input")
    session:resize(100, 30)

    test.equal(calls.attach, 1)
    test.same(calls.replay, { 0 })
    test.same(calls.writes, { "input" })
    test.same(calls.resizes, { { 100, 30 } })
    test.ok(contains_terminal_text(view.terminal, "remote session"))
    test.equal(session:offset(), #"remote session\r\n")

    view:close()
    test.equal(calls.detach, 1)
    test.equal(session:status(), "running")
    emulator:close()
  end)

  test.test("drains output that arrives after an exit event", function()
    test.skip_if(not available, "terminal plugin is unavailable: " .. tostring(terminal))
    local pending = {
      {
        { type = "output", runtime_id = "drain-conformance", offset = 0,
          data = "before exit\r\n" },
        { type = "status", runtime_id = "drain-conformance", status = "exited",
          exit_code = 7 }
      },
      {
        { type = "output", runtime_id = "drain-conformance",
          offset = #"before exit\r\n", data = "after exit\r\n" }
      }
    }
    local emulator = terminal.new_emulator { columns = 20, rows = 4 }
    local session = terminal.Session {
      id = "drain-conformance",
      emulator = emulator,
      capabilities = { events_applied = false },
      write = function() end,
      resize = function() end,
      detach = function() end,
      poll_events = function()
        local events = table.remove(pending, 1)
        return events or {}
      end
    }
    local view = terminal.class { session = session, emulator = emulator }

    view:shift_selection_update()
    test.equal(session:status(), "exited")
    test.equal(view.exit_event.exit_code, 7)
    view:shift_selection_update()

    test.ok(contains_terminal_text(view.terminal, "before exit"))
    test.ok(contains_terminal_text(view.terminal, "after exit"))
    test.equal(session:offset(), #"before exit\r\n" + #"after exit\r\n")
    view:close()
    emulator:close()
  end)

  test.test("keeps detach, terminate, and close distinct and idempotent", function()
    test.skip_if(not available, "terminal plugin is unavailable: " .. tostring(terminal))
    local calls = { attach = 0, detach = 0, terminate = {}, close = 0 }
    local session = terminal.Session {
      id = "lifecycle-conformance",
      write = function() end,
      resize = function() end,
      attach = function() calls.attach = calls.attach + 1 end,
      detach = function() calls.detach = calls.detach + 1 end,
      terminate = function(_, options)
        calls.terminate[#calls.terminate + 1] = options
      end,
      close = function() calls.close = calls.close + 1 end,
      poll_events = function() return {} end
    }

    session:attach()
    session:detach()
    session:detach()
    session:attach()
    session:detach()
    session:terminate { force = true }
    session:close()
    session:close()
    session:detach()

    test.equal(calls.attach, 2)
    test.equal(calls.detach, 2)
    test.same(calls.terminate, { { force = true } })
    test.equal(calls.close, 1)
    test.equal(session:status(), "closed")
  end)
end)
