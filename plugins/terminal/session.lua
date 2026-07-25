-- Backend-independent terminal session contract.

local Session = {}
Session.__index = Session

local function required_method(options, name)
  local method = options[name]
  if type(method) ~= "function" then
    error("terminal session requires a " .. name .. " function")
  end
  return method
end

function Session.new(options)
  options = options or {}
  local self = setmetatable({}, Session)
  self._id = options.id or error("terminal session requires an id")
  self._status = options.status or "starting"
  self._write = required_method(options, "write")
  self._resize = required_method(options, "resize")
  self._poll_events = options.poll_events or options.poll
  if type(self._poll_events) ~= "function" then
    error("terminal session requires a poll_events function")
  end
  self._terminate = options.terminate
  self._request_replay = options.request_replay
  self._detach = options.detach
  self._close = options.close
  self.emulator = options.emulator
  self.capabilities = options.capabilities or {}
  self.last_status_event = nil
  self.last_shifts = 0
  self._closed = false
  return self
end

function Session:id() return self._id end
function Session:status() return self._status end
function Session:set_status(status) self._status = status end

function Session:write(data)
  if type(data) ~= "string" then error("terminal session data must be a string") end
  return self._write(self, data)
end

function Session:resize(columns, rows)
  return self._resize(self, columns, rows)
end

function Session:terminate(options)
  if not self._terminate then return false, "session does not support termination" end
  if self._closed then return true end
  return self._terminate(self, options or {})
end

function Session:request_replay(offset)
  if not self._request_replay then return false, "session does not support replay" end
  offset = offset or 0
  if type(offset) ~= "number" or offset < 0 then
    return false, "session replay offset must be a non-negative number"
  end
  return self._request_replay(self, offset)
end

function Session:detach()
  if self._detach then return self._detach(self) end
  return true
end

function Session:poll_events()
  local events = self._poll_events(self) or {}
  for _, event in ipairs(events) do
    if event.type == "status" and event.status then
      self._status = event.status
      self.last_status_event = event
    end
  end
  return events
end

function Session:poll()
  return self:poll_events()
end

function Session:close()
  if self._closed then return true end
  local result, message
  if self._close then
    result, message = self._close(self)
  else
    result, message = self:terminate()
  end
  if result ~= false then
    self._closed = true
    self._status = "closed"
  end
  return result, message
end

return setmetatable(Session, {
  __call = function(_, options) return Session.new(options) end
})
