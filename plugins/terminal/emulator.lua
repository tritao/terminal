-- Lua-facing terminal emulator factory.  The native parser is currently
-- exposed through libterminal; this wrapper keeps it separate from sessions
-- and provides the stable frontend boundary used by remote adapters.

local terminal_native = require "plugins.terminal.libterminal"

local Emulator = {}
Emulator.__index = Emulator

local function environment_for_native(environment)
  environment = environment or {}
  if PLATFORM == "Windows" then
    local values = {}
    local defaults = terminal_native.getenv and terminal_native.getenv() or {}
    for key, value in pairs(defaults) do values[key] = value end
    for key, value in pairs(environment) do values[key] = value end
    local result = {}
    for key, value in pairs(values) do result[#result + 1] = key .. "=" .. value end
    return table.concat(result, "\0") .. "\0\0"
  end
  return environment
end

function Emulator.new(options)
  options = options or {}
  local native = terminal_native.new(
    options.columns or 80,
    options.rows or 24,
    options.scrollback or options.scrollback_limit or 10000,
    options.term or "xterm-256color",
    "DUMMY",
    {},
    environment_for_native(options.environment),
    options.debug or false
  )
  return setmetatable({ native = native }, Emulator)
end

function Emulator.from_native(native)
  if not native then return nil end
  return setmetatable({ native = native }, Emulator)
end

function Emulator:feed(data)
  if type(data) ~= "string" then error("terminal emulator data must be a string") end
  return self.native:input(data) or 0
end
function Emulator:checkpoint() return self.native:checkpoint() end
function Emulator:restore_checkpoint(data)
  if type(data) ~= "string" then error("terminal checkpoint data must be a string") end
  return self.native:restore_checkpoint(data)
end
function Emulator:size(...) return self.native:size(...) end
function Emulator:resize(columns, rows) return self.native:size(columns, rows) end
function Emulator:update(callback)
  local shifts, data = self.native:update()
  if callback and data ~= "" then callback(data) end
  return shifts
end
function Emulator:lines(...) return self.native:lines(...) end
function Emulator:cursor(...) return self.native:cursor(...) end
function Emulator:scrollback(...) return self.native:scrollback(...) end
function Emulator:clear() return self.native:clear() end
function Emulator:clear_scrollback()
  if self.native.clear_scrollback then return self.native:clear_scrollback() end
  return self.native:clear()
end
function Emulator:reset()
  if self.native.reset then return self.native:reset() end
  return self.native:clear()
end
function Emulator:focused(focused) return self.native:focused(focused) end
function Emulator:mouse_tracking_mode() return self.native:mouse_tracking_mode() end
function Emulator:mouse_encoding() return self.native:mouse_encoding() end
function Emulator:synchronized_output()
  return self.native.synchronized_output and self.native:synchronized_output() or false
end
function Emulator:keyboard(key_name, modifiers, unicode)
  return self.native.keyboard and self.native:keyboard(key_name, modifiers or 0, unicode) or false
end
function Emulator:mouse(col, row, button, event, modifiers)
  return self.native.mouse and self.native:mouse(col, row, button, event, modifiers) or false
end
function Emulator:cursor_keys_mode() return self.native:cursor_keys_mode() end
function Emulator:keypad_keys_mode() return self.native:keypad_keys_mode() end
function Emulator:paste_mode() return self.native:paste_mode() end
function Emulator:name() return self.native:name() end
function Emulator:close() return self.native:close() end

return setmetatable(Emulator, {
  __call = function(_, options) return Emulator.new(options) end
})
