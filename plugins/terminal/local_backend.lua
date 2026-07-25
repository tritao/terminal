local Session = require "plugins.terminal.session"
local Emulator = require "plugins.terminal.emulator"
local terminal_native = require "plugins.terminal.libterminal"

local LocalSession = {}
LocalSession.__index = LocalSession
local next_id = 0

local function make_id()
  next_id = next_id + 1
  return "local-terminal-" .. tostring(math.floor(system.get_time() * 1000000))
    .. "-" .. tostring(next_id)
end

local function merge_environment(environment)
  environment = environment or {}
  if PLATFORM ~= "Windows" then
    local result = {}
    for key, value in pairs(environment) do result[key] = value end
    return result
  end
  local values = terminal_native.getenv and terminal_native.getenv() or {}
  for key, value in pairs(environment) do values[key] = value end
  local result = {}
  for key, value in pairs(values) do result[#result + 1] = key .. "=" .. value end
  return table.concat(result, "\0") .. "\0\0"
end

function LocalSession.new(options)
  options = options or {}
  local command = options.command or options.shell
  if not command then error("local terminal session requires a command") end

  local ok, runtime = pcall(terminal_native.new,
    options.columns or 80,
    options.rows or 24,
    options.scrollback or options.scrollback_limit or 10000,
    options.term or "xterm-256color",
    command,
    options.args or options.arguments or {},
    merge_environment(options.environment),
    options.debug or false,
    options.cwd
  )
  if not ok then error(runtime) end

  local self = setmetatable({
    runtime = runtime,
    status_name = "running",
    offset = 0,
    terminate_on_detach = options.terminate_on_detach ~= false,
    emulator = Emulator.from_native(runtime)
  }, LocalSession)
  local function close_runtime(terminate_options)
    if not self.runtime then return true end
    local result = self.runtime:close(terminate_options)
    self.runtime = nil
    self.status_name = "closed"
    return result
  end
  self.session = Session {
    id = options.id or make_id(),
    status = "running",
    emulator = self.emulator,
    capabilities = { local_process = true, replay = false,
      persistent = not self.terminate_on_detach, events_applied = true },
    write = function(_, data) return runtime:input(data) end,
    resize = function(_, columns, rows) return runtime:size(columns, rows) end,
    terminate = function(_, terminate_options)
      self.session:set_status("closed")
      return close_runtime(terminate_options)
    end,
    detach = function()
      if self.terminate_on_detach and self.status_name ~= "closed" then
        self.session:set_status("closed")
        return close_runtime()
      end
      return true
    end,
    close = function()
      self.session:set_status("closed")
      return close_runtime()
    end,
    poll_events = function() return self:poll_events() end
  }
  return self.session
end

function LocalSession:poll_events()
  local events = {}
  if not self.runtime then
    return events
  end
  local output_offset = self.offset
  local result, data = self.runtime:update()
  if data and data ~= "" then
    self.offset = self.offset + #data
    events[#events + 1] = {
      type = "output", runtime_id = self.session:id(),
      offset = output_offset, data = data
    }
    output_offset = self.offset
  end
  self.session.last_shifts = type(result) == "number" and result or 0

  local exited = self.runtime:exited()
  if exited ~= false and self.status_name == "running" then
    local exit_code, signal = exited, nil
    if type(exited) == "table" then exit_code, signal = table.unpack(exited) end
    self.status_name = "exited"
    self.session:set_status("exited")
    events[#events + 1] = {
      type = "status", runtime_id = self.session:id(), status = "exited",
      exit_code = exit_code, signal = signal
    }
  end
  return events
end

return setmetatable(LocalSession, {
  __call = function(_, options) return LocalSession.new(options) end
})
