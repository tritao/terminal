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

test.describe("local terminal backend", function()
  local function collect_session_output(session)
    local output = {}
    local deadline = system.get_time() + 2
    while system.get_time() < deadline and session:status() == "running" do
      for _, event in ipairs(session:poll_events()) do
        if event.type == "output" then output[#output + 1] = event.data end
      end
      system.sleep(0.01)
    end
    session:close()
    return table.concat(output)
  end

  test.test("removes inherited NO_COLOR unless explicitly configured", function()
    test.skip_if(PLATFORM == "Windows", "POSIX shell expansion is required")
    local LocalSession = require "plugins.terminal.local_backend"
    local session = LocalSession {
      command = "/bin/sh",
      args = { "-c", "printf '%s' \"${NO_COLOR-unset}\"" },
    }
    test.equal(collect_session_output(session), "unset")

    session = LocalSession {
      command = "/bin/sh",
      args = { "-c", "printf '%s' \"${NO_COLOR-unset}\"" },
      environment = { NO_COLOR = "explicit" },
    }
    test.equal(collect_session_output(session), "explicit")
  end)

  test.test("launches, polls, and closes a POSIX PTY session", function()
    test.skip_if(PLATFORM == "Windows", "POSIX PTY launch is required")
    local LocalSession = require "plugins.terminal.local_backend"
    local original_cwd = system.getcwd()
    local session = LocalSession {
      command = "/bin/sh", args = { "-c", "printf terminal-local-output; pwd" },
      cwd = "/tmp", columns = 80, rows = 24
    }
    test.equal(system.getcwd(), original_cwd)
    test.equal(session:status(), "running")
    local output = {}
    local deadline = system.get_time() + 2
    while system.get_time() < deadline and session:status() == "running" do
      local events = session:poll_events()
      for _, event in ipairs(events) do
        if event.type == "output" then output[#output + 1] = event.data end
      end
      system.sleep(0.01)
    end
    test.contains(table.concat(output), "terminal-local-output")
    test.contains(table.concat(output), "/tmp")
    local result = session:close()
    test.ok(result == true or result == 0 or session:status() == "closed")
    test.equal(session:status(), "closed")
  end)

  test.test("renders a local session through TerminalView until exit", function()
    test.skip_if(PLATFORM == "Windows", "POSIX PTY launch is required")
    test.skip_if(not available, "terminal plugin is unavailable: " .. tostring(terminal))
    local LocalSession = require "plugins.terminal.local_backend"
    local session = LocalSession {
      command = "/bin/sh",
      args = { "-c", "printf local-view-output; exit 7" },
      cwd = "/tmp", columns = 80, rows = 24
    }
    local view = terminal.class { session = session, emulator = session.emulator }
    local deadline = system.get_time() + 2
    local idle_after_exit = 0
    while system.get_time() < deadline and idle_after_exit < 3 do
      view:shift_selection_update()
      if session:status() == "exited" then
        idle_after_exit = idle_after_exit + 1
      else
        idle_after_exit = 0
      end
      system.sleep(0.01)
    end
    test.equal(session:status(), "exited")
    test.equal(view.exit_event.exit_code, 7)
    test.ok(contains_terminal_text(view.terminal, "local-view-output"))
    view:close()
    test.equal(session:status(), "closed")
  end)

  test.test("detaches without terminating a persistent local session", function()
    test.skip_if(PLATFORM == "Windows", "POSIX PTY launch is required")
    test.skip_if(not available, "terminal plugin is unavailable: " .. tostring(terminal))
    local LocalSession = require "plugins.terminal.local_backend"
    local session = LocalSession {
      command = "/bin/sh", args = { "-c", "sleep 1" },
      cwd = "/tmp", columns = 80, rows = 24,
      terminate_on_detach = false
    }
    local view = terminal.class { session = session, emulator = session.emulator }
    view:close()
    test.equal(session:status(), "running")
    session:terminate()
    test.equal(session:status(), "closed")
  end)
end)
