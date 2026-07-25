local test = require "core.test"

test.describe("local terminal backend", function()
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
end)
