-- Backend-independent terminal session contract.

local Session = {}
Session.__index = Session

local function valid_offset(offset)
  return type(offset) == "number"
    and offset >= 0
    and offset == math.floor(offset)
end

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
  local offset = options.offset
  if offset == nil then offset = options.last_seen_offset or 0 end
  if not valid_offset(offset) then
    error("terminal session offset must be a non-negative integer")
  end
  self._terminate = options.terminate
  self._attach = options.attach
  self._request_replay = options.request_replay
  self._restore_checkpoint = options.restore_checkpoint
  self._detach = options.detach
  self._close = options.close
  self.emulator = options.emulator
  self.capabilities = options.capabilities or {}
  self.last_status_event = nil
  self.last_shifts = 0
  self._offset = offset
  self._replay_pending = nil
  self._attached = false
  self.duplicate_events = 0
  self.gap_events = 0
  self._closed = false
  return self
end

function Session:id() return self._id end
function Session:status() return self._status end
function Session:set_status(status) self._status = status end
function Session:offset() return self._offset end
function Session:replay_pending() return self._replay_pending end

function Session:attach()
  if self._attached then return true end
  if self._attach then
    local result, message = self._attach(self)
    if result == false then return result, message end
  end
  self._attached = true
  if self.capabilities.replay and self._request_replay and not self._replay_pending then
    local result, message = self:request_replay(self._offset)
    if result == false then
      self._attached = false
      return result, message
    end
  end
  return true
end

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
  if not valid_offset(offset) then
    return false, "session replay offset must be a non-negative integer"
  end
  self._replay_pending = offset
  local result, message = self._request_replay(self, offset)
  if result == false then self._replay_pending = nil end
  return result == nil and true or result, message
end

function Session:apply_checkpoint(event, emulator)
  if type(event) ~= "table" or event.type ~= "checkpoint" then
    return false, "invalid terminal checkpoint event"
  end
  if not valid_offset(event.offset) then
    return false, "terminal checkpoint requires a non-negative integer offset"
  end
  if event.offset < self._offset then
    return false, "terminal checkpoint is older than the current offset"
  end

  local restored = event.emulator
  local handled = restored ~= nil
  local message
  if not restored and event.restore then
    handled = true
    restored, message = event.restore(event, emulator)
  end
  if not restored and self._restore_checkpoint then
    handled = true
    restored, message = self._restore_checkpoint(self, event, emulator)
  end
  if not handled and event.data == nil then
    handled = true
    restored = emulator
  end
  if not handled and emulator and emulator.restore_checkpoint and event.data then
    handled = true
    restored, message = emulator:restore_checkpoint(event.data)
  end
  if not handled then
    return false, message or "terminal checkpoint has no restore handler"
  end
  if restored == false then
    return false, message or "terminal checkpoint restore failed"
  end
  if restored == true or restored == nil then restored = emulator end
  if not restored then
    return false, message or "terminal checkpoint has no restore handler"
  end

  self._offset = event.offset
  self._replay_pending = nil
  return restored
end

function Session:detach()
  local result, message = true
  if self._detach then result, message = self._detach(self) end
  if result ~= false then
    self._attached = false
    self._replay_pending = nil
  end
  return result, message
end

function Session:poll_events()
  local received = self._poll_events(self) or {}
  local events = {}
  for _, event in ipairs(received) do
    if type(event) == "table" and event.type == "output" then
      local data = event.data
      if type(data) ~= "string" then
        events[#events + 1] = {
          type = "error", runtime_id = self._id,
          message = "terminal output event requires string data"
        }
      else
        local offset = event.offset
        if offset == nil then
          offset = self._offset
          event.offset = offset
        end
        if not valid_offset(offset) then
          events[#events + 1] = {
            type = "error", runtime_id = self._id,
            message = "terminal output event requires a non-negative integer offset"
          }
        elseif offset < self._offset then
          self.duplicate_events = self.duplicate_events + 1
        elseif offset > self._offset then
          self.gap_events = self.gap_events + 1
          events[#events + 1] = {
            type = "gap", runtime_id = self._id,
            offset = self._offset, received_offset = offset
          }
          if self.capabilities.replay and self._request_replay
              and not self._replay_pending then
            self:request_replay(self._offset)
          end
        else
          events[#events + 1] = event
          self._offset = offset + #data
          self._replay_pending = nil
        end
      end
    elseif type(event) == "table" and event.type == "checkpoint" then
      if not valid_offset(event.offset) then
        events[#events + 1] = {
          type = "error", runtime_id = self._id,
          message = "terminal checkpoint requires a non-negative integer offset"
        }
      elseif event.offset < self._offset then
        self.duplicate_events = self.duplicate_events + 1
      else
        events[#events + 1] = event
        self._offset = event.offset
        self._replay_pending = nil
      end
    elseif type(event) ~= "table" then
      events[#events + 1] = {
        type = "error", runtime_id = self._id,
        message = "invalid terminal session event"
      }
    else
      events[#events + 1] = event
      if event.type == "status" and event.status then
        self._status = event.status
        self.last_status_event = event
      end
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
