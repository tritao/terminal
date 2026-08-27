-- mod-version:3
local core = require "core"
local config = require "core.config"
local command = require "core.command"
local style = require "core.style"
local common = require "core.common"
local View = require "core.view"
local keymap = require "core.keymap"
local StatusView = require "core.statusview"

local Session = require "plugins.terminal.session"
local Emulator = require "plugins.terminal.emulator"
local LocalSession = require "plugins.terminal.local_backend"

local prev_scale = SCALE
local default_shell =  os.getenv("SHELL") or (PLATFORM == "Windows" and os.getenv("COMSPEC")) or (PLATFORM == "Windows" and "c:\\windows\\system32\\cmd.exe" or "sh")
local default_config = {
  -- outputs a terminal.log file of all the output of your shell
  debug = false,
  -- the TERM to present as.
  term = "xterm-256color",
  -- pressing this key and ctrl will allow normal commands to be run that start with ctrl (ctrl+n, ctrl+w, etc..) while using the terminal
  -- set to nil to disable entirely
  inversion_key = "shift",
  -- lua pattern for escape sequences to ignore. nil to target all escapes to terminal. example value would be "ctrl%+[nwpf]"
  omit_escapes = nil,
  -- the default shell to boot up in
  shell = default_shell,
  -- the arguments to pass to your shell; does not work on windows.
  arguments = { },
  -- the environmental variable to set on shell instantiation
  environment = { },
  -- the amount of time between line scrolling when we're dragging offscreen
  scrolling_speed = 0.01,
  -- the newline character to use
  -- We set ICRNL, so that this is translated to `\n` at input time... but this seems to be necessary. `micro`
  -- doesn't allow you to newline if you don't set it to `\r`.
  newline = ((config.plugins.terminal.shell or default_shell):find("cmd.exe") and "\r\n" or "\r"),
  -- the backspace character to use
  backspace = "\x7F",
  -- the delete character to use
  delete = "\x1B[3~",
  -- the amount of lines you can emit before we start cutting them off
  scrollback_limit = 10000,
  profiles = {
    default = { command = default_shell },
    login = { command = default_shell, args = { "--login" } }
  },
  -- the default height of the console drawer
  drawer_height = 300,
  -- Use a different font from the code editor
  use_custom_font = false,
  -- the default console font. non-monospace is unsupported, fonts that
  -- work great and provide mostly everything you need to properly render
  -- things like btop:
  -- * JuliaMono Regular - https://github.com/cormullion/juliamono
  -- * MesloLGS NF Regular - https://github.com/romkatv/powerlevel10k-media
  -- You should use both, Julia as main and MesloLGS as fallback.
  font = style.code_font,
  bold_font = style.code_font:copy(style.code_font:get_size(), { smoothing = true }),
  -- padding around the edges of the terminal
  padding = { x = 0, y = 0 },
  -- default background color if not explicitly set by the shell
  background = style.background or { common.color "#000000" },
  -- default text color if not explicitly by the shell
  text = style.syntax and style.syntax.normal or { common.color "#FFFFFF" },
  -- show bold text in bright colors
  bold_text_in_bright_colors = true,
  -- set to 0 to disable color adjustments based on background
  minimum_contrast_ratio = 3,
  colors = {
    -- You can customize these without many repercussions.
    [  0] = { common.color "#000000" }, [  1] = { common.color "#aa0000" }, [  2] = { common.color "#44aa44" }, [  3] = { common.color "#aa5500" }, [  4] = { common.color "#0039aa" },
    [  5] = { common.color "#aa22aa" }, [  6] = { common.color "#1a92aa" }, [  7] = { common.color "#aaaaaa" }, [  8] = { common.color "#777777" }, [  9] = { common.color "#ff8787" },
    [ 10] = { common.color "#4ce64c" }, [ 11] = { common.color "#ded82c" }, [ 12] = { common.color "#295fcc" }, [ 13] = { common.color "#cc58cc" }, [ 14] = { common.color "#4ccce6" },
    [ 15] = { common.color "#ffffff" },
    -- You can't customize these without repercussions.
    [ 16] = { common.color "#000000" }, [ 17] = { common.color "#00005f" }, [ 18] = { common.color "#000087" }, [ 19] = { common.color "#0000af" },
    [ 20] = { common.color "#0000d7" }, [ 21] = { common.color "#0000ff" }, [ 22] = { common.color "#005f00" }, [ 23] = { common.color "#005f5f" }, [ 24] = { common.color "#005f87" },
    [ 25] = { common.color "#005faf" }, [ 26] = { common.color "#005fd7" }, [ 27] = { common.color "#005fff" }, [ 28] = { common.color "#008700" }, [ 29] = { common.color "#00875f" },
    [ 30] = { common.color "#008787" }, [ 31] = { common.color "#0087af" }, [ 32] = { common.color "#0087d7" }, [ 33] = { common.color "#0087ff" }, [ 34] = { common.color "#00af00" },
    [ 35] = { common.color "#00af5f" }, [ 36] = { common.color "#00af87" }, [ 37] = { common.color "#00afaf" }, [ 38] = { common.color "#00afd7" }, [ 39] = { common.color "#00afff" },
    [ 40] = { common.color "#00d700" }, [ 41] = { common.color "#00d75f" }, [ 42] = { common.color "#00d787" }, [ 43] = { common.color "#00d7af" }, [ 44] = { common.color "#00d7d7" },
    [ 45] = { common.color "#00d7ff" }, [ 46] = { common.color "#00ff00" }, [ 47] = { common.color "#00ff5f" }, [ 48] = { common.color "#00ff87" }, [ 49] = { common.color "#00ffaf" },
    [ 50] = { common.color "#00ffd7" }, [ 51] = { common.color "#00ffff" }, [ 52] = { common.color "#5f0000" }, [ 53] = { common.color "#5f005f" }, [ 54] = { common.color "#5f0087" },
    [ 55] = { common.color "#5f00af" }, [ 56] = { common.color "#5f00d7" }, [ 57] = { common.color "#5f00ff" }, [ 58] = { common.color "#5f5f00" }, [ 59] = { common.color "#5f5f5f" },
    [ 60] = { common.color "#5f5f87" }, [ 61] = { common.color "#5f5faf" }, [ 62] = { common.color "#5f5fd7" }, [ 63] = { common.color "#5f5fff" }, [ 64] = { common.color "#5f8700" },
    [ 65] = { common.color "#5f875f" }, [ 66] = { common.color "#5f8787" }, [ 67] = { common.color "#5f87af" }, [ 68] = { common.color "#5f87d7" }, [ 69] = { common.color "#5f87ff" },
    [ 70] = { common.color "#5faf00" }, [ 71] = { common.color "#5faf5f" }, [ 72] = { common.color "#5faf87" }, [ 73] = { common.color "#5fafaf" }, [ 74] = { common.color "#5fafd7" },
    [ 75] = { common.color "#5fafff" }, [ 76] = { common.color "#5fd700" }, [ 77] = { common.color "#5fd75f" }, [ 78] = { common.color "#5fd787" }, [ 79] = { common.color "#5fd7af" },
    [ 80] = { common.color "#5fd7d7" }, [ 81] = { common.color "#5fd7ff" }, [ 82] = { common.color "#5fff00" }, [ 83] = { common.color "#5fff5f" }, [ 84] = { common.color "#5fff87" },
    [ 85] = { common.color "#5fffaf" }, [ 86] = { common.color "#5fffd7" }, [ 87] = { common.color "#5fffff" }, [ 88] = { common.color "#870000" }, [ 89] = { common.color "#87005f" },
    [ 90] = { common.color "#870087" }, [ 91] = { common.color "#8700af" }, [ 92] = { common.color "#8700d7" }, [ 93] = { common.color "#8700ff" }, [ 94] = { common.color "#875f00" },
    [ 95] = { common.color "#875f5f" }, [ 96] = { common.color "#875f87" }, [ 97] = { common.color "#875faf" }, [ 98] = { common.color "#875fd7" }, [ 99] = { common.color "#875fff" },
    [100] = { common.color "#878700" }, [101] = { common.color "#87875f" }, [102] = { common.color "#878787" }, [103] = { common.color "#8787af" }, [104] = { common.color "#8787d7" },
    [105] = { common.color "#8787ff" }, [106] = { common.color "#87af00" }, [107] = { common.color "#87af5f" }, [108] = { common.color "#87af87" }, [109] = { common.color "#87afaf" },
    [110] = { common.color "#87afd7" }, [111] = { common.color "#87afff" }, [112] = { common.color "#87d700" }, [113] = { common.color "#87d75f" }, [114] = { common.color "#87d787" },
    [115] = { common.color "#87d7af" }, [116] = { common.color "#87d7d7" }, [117] = { common.color "#87d7ff" }, [118] = { common.color "#87ff00" }, [119] = { common.color "#87ff5f" },
    [120] = { common.color "#87ff87" }, [121] = { common.color "#87ffaf" }, [122] = { common.color "#87ffd7" }, [123] = { common.color "#87ffff" }, [124] = { common.color "#af0000" },
    [125] = { common.color "#af005f" }, [126] = { common.color "#af0087" }, [127] = { common.color "#af00af" }, [128] = { common.color "#af00d7" }, [129] = { common.color "#af00ff" },
    [130] = { common.color "#af5f00" }, [131] = { common.color "#af5f5f" }, [132] = { common.color "#af5f87" }, [133] = { common.color "#af5faf" }, [134] = { common.color "#af5fd7" },
    [135] = { common.color "#af5fff" }, [136] = { common.color "#af8700" }, [137] = { common.color "#af875f" }, [138] = { common.color "#af8787" }, [139] = { common.color "#af87af" },
    [140] = { common.color "#af87d7" }, [141] = { common.color "#af87ff" }, [142] = { common.color "#afaf00" }, [143] = { common.color "#afaf5f" }, [144] = { common.color "#afaf87" },
    [145] = { common.color "#afafaf" }, [146] = { common.color "#afafd7" }, [147] = { common.color "#afafff" }, [148] = { common.color "#afd700" }, [149] = { common.color "#afd75f" },
    [150] = { common.color "#afd787" }, [151] = { common.color "#afd7af" }, [152] = { common.color "#afd7d7" }, [153] = { common.color "#afd7ff" }, [154] = { common.color "#afff00" },
    [155] = { common.color "#afff5f" }, [156] = { common.color "#afff87" }, [157] = { common.color "#afffaf" }, [158] = { common.color "#afffd7" }, [159] = { common.color "#afffff" },
    [160] = { common.color "#d70000" }, [161] = { common.color "#d7005f" }, [162] = { common.color "#d70087" }, [163] = { common.color "#d700af" }, [164] = { common.color "#d700d7" },
    [165] = { common.color "#d700ff" }, [166] = { common.color "#d75f00" }, [167] = { common.color "#d75f5f" }, [168] = { common.color "#d75f87" }, [169] = { common.color "#d75faf" },
    [170] = { common.color "#d75fd7" }, [171] = { common.color "#d75fff" }, [172] = { common.color "#d78700" }, [173] = { common.color "#d7875f" }, [174] = { common.color "#d78787" },
    [175] = { common.color "#d787af" }, [176] = { common.color "#d787d7" }, [177] = { common.color "#d787ff" }, [178] = { common.color "#d7af00" }, [179] = { common.color "#d7af5f" },
    [180] = { common.color "#d7af87" }, [181] = { common.color "#d7afaf" }, [182] = { common.color "#d7afd7" }, [183] = { common.color "#d7afff" }, [184] = { common.color "#d7d700" },
    [185] = { common.color "#d7d75f" }, [186] = { common.color "#d7d787" }, [187] = { common.color "#d7d7af" }, [188] = { common.color "#d7d7d7" }, [189] = { common.color "#d7d7ff" },
    [190] = { common.color "#d7ff00" }, [191] = { common.color "#d7ff5f" }, [192] = { common.color "#d7ff87" }, [193] = { common.color "#d7ffaf" }, [194] = { common.color "#d7ffd7" },
    [195] = { common.color "#d7ffff" }, [196] = { common.color "#ff0000" }, [197] = { common.color "#ff005f" }, [198] = { common.color "#ff0087" }, [199] = { common.color "#ff00af" },
    [200] = { common.color "#ff00d7" }, [201] = { common.color "#ff00ff" }, [202] = { common.color "#ff5f00" }, [203] = { common.color "#ff5f5f" }, [204] = { common.color "#ff5f87" },
    [205] = { common.color "#ff5faf" }, [206] = { common.color "#ff5fd7" }, [207] = { common.color "#ff5fff" }, [208] = { common.color "#ff8700" }, [209] = { common.color "#ff875f" },
    [210] = { common.color "#ff8787" }, [211] = { common.color "#ff87af" }, [212] = { common.color "#ff87d7" }, [213] = { common.color "#ff87ff" }, [214] = { common.color "#ffaf00" },
    [215] = { common.color "#ffaf5f" }, [216] = { common.color "#ffaf87" }, [217] = { common.color "#ffafaf" }, [218] = { common.color "#ffafd7" }, [219] = { common.color "#ffafff" },
    [220] = { common.color "#ffd700" }, [221] = { common.color "#ffd75f" }, [222] = { common.color "#ffd787" }, [223] = { common.color "#ffd7af" }, [224] = { common.color "#ffd7d7" },
    [225] = { common.color "#ffd7ff" }, [226] = { common.color "#ffff00" }, [227] = { common.color "#ffff5f" }, [228] = { common.color "#ffff87" }, [229] = { common.color "#ffffaf" },
    [230] = { common.color "#ffffd7" }, [231] = { common.color "#ffffff" }, [232] = { common.color "#080808" }, [233] = { common.color "#121212" }, [234] = { common.color "#1c1c1c" },
    [235] = { common.color "#262626" }, [236] = { common.color "#303030" }, [237] = { common.color "#3a3a3a" }, [238] = { common.color "#444444" }, [239] = { common.color "#4e4e4e" },
    [240] = { common.color "#585858" }, [241] = { common.color "#626262" }, [242] = { common.color "#6c6c6c" }, [243] = { common.color "#767676" }, [244] = { common.color "#808080" },
    [245] = { common.color "#8a8a8a" }, [246] = { common.color "#949494" }, [247] = { common.color "#9e9e9e" }, [248] = { common.color "#a8a8a8" }, [249] = { common.color "#b2b2b2" },
    [250] = { common.color "#bcbcbc" }, [251] = { common.color "#c6c6c6" }, [252] = { common.color "#d0d0d0" }, [253] = { common.color "#dadada" }, [254] = { common.color "#e4e4e4" },
    [255] = { common.color "#eeeeee" }
  },
}
local function set_config_default_values(c) for _, v in ipairs(c) do if v.path then v.default = default_config[v.path] end end return c end
-- configuration for the settings GUI (do not modify)
default_config.config_spec = set_config_default_values {
  name = "Terminal",
  {
    label = "Use Custom Font",
    description = "Use the configured custom font for the terminal (requires restart).",
    path = "use_custom_font", type = "TOGGLE",
    default = false
  },
  {
    label = "Font",
    description = "Custom font to use for the terminal (non-monospace unsupported, great choices are JuliaMono and MesloLGS NF) (requires restart).",
    path = "font", type = "font",
    default = {
      fonts = {
        {
          name = "JetBrains Mono Regular",
          path = DATADIR .. "/fonts/JetBrainsMono-Regular.ttf"
        }
      },
      options = {
        size = 15,
        antialiasing = "subpixel",
        hinting = "slight"
      }
    },
    on_apply = function()
      prev_scale = SCALE
      if not config.plugins.terminal.use_custom_font then
        config.plugins.terminal.font = style.code_font
      end
      config.plugins.terminal.bold_font = config.plugins.terminal.font:copy(
        config.plugins.terminal.font:get_size(), { smoothing = true }
      )
    end
  },
  {
    label = "Background Color",
    description = "The color of the terminal background (when not overridden by the shell).",
    path = "background", type = "COLOR",
    default = style.background or { common.color "#000000" }
  },
  {
    label = "Text Color",
    description = "The color of the text (when not overridden by the shell).",
    path = "text", type = "COLOR",
    default = style.syntax and style.syntax.normal or { common.color "#FFFFFF" }
  },
  {
    label = "Show Bold Text In Bright Colors",
    description = "Display emboldened text with brighter colors.",
    path = "bold_text_in_bright_colors", type = "TOGGLE",
    default = true
  },
  {
    label = "Minimum Contrast Ratio",
    description = "Minimum contrast between the text and background color (set to 0 to disable auto color adjustment).",
    path = "minimum_contrast_ratio", type = "NUMBER",
    default = 3
  },
  {
    label = "Terminal Drawer Height",
    description = "Height of the terminal drawer (in pixels).",
    path = "drawer_height", type = "NUMBER",
    default = 300
  },
  {
    label = "Inversion Key",
    description = "The key to press in combination with CTRL to send the shortcut to Pragtical instead of the terminal.",
    path = "inversion_key", type = "STRING",
    default = "shift"
  },
  {
    label = "Omit Shortcuts",
    description = "A Lua pattern of shortcuts to pass to Pragtical instead of the terminal.",
    path = "omit_escapes", type = "STRING"
  },
  {
    label = "Scrolling speed",
    description = "The amount of time to scroll a line when selecting text offscreen.",
    path = "scrolling_speed", type = "NUMBER",
    default = 0.01
  },
  {
    label = "Shell",
    description = "Absolute path to the shell for the terminal.",
    path = "shell", type = "FILE", exists=true,
    default = default_shell,
    on_apply = function(shell)
      if shell:lower():find("cmd.exe") then
        config.plugins.terminal.newline = "\r\n"
      else
        config.plugins.terminal.newline = "\r"
      end
    end
  },
  {
    label = "Shell Arguments",
    description = "Extra arguments to pass to the shell; does not function on windows.",
    path = "arguments", type = "LIST_STRINGS"
  },
  {
    label = "Terminal Type",
    description = "The type to terminal to appear as (sets the $TERM environment variable).",
    path = "term", type = "STRING",
    default = "xterm-256color"
  },
  {
    label = "Scrollback Buffer Size",
    description = "Number of lines to store for scrolling in the terminal.",
    path = "scrollback_limit", type = "NUMBER",
    default = 10000
  },
  {
    label = "Change Other Options",
    description = "For other options such as the color palette, you can change them in the user module.",
    icon = "P", type = "BUTTON", on_click = "core:open-user-module", path = ""
  }
}
local user_terminal_config = config.plugins.terminal
config.plugins.terminal = common.merge(default_config, user_terminal_config)
-- Keep the built-in palette when only a few colors are customized.  The
-- generic merge helper is intentionally shallow, but replacing the complete
-- palette makes otherwise valid indexed colors resolve to nil.
if type(user_terminal_config) == "table"
  and type(user_terminal_config.colors) == "table" then
  config.plugins.terminal.colors = common.merge(
    default_config.colors, user_terminal_config.colors
  )
end

core.add_thread(function()
  -- give time for ui settings (even on editor restart) and then apply font
  for _=1, 2 do coroutine.yield() end
  if not config.plugins.terminal.use_custom_font then
    config.plugins.terminal.font = style.code_font
    config.plugins.terminal.bold_font = config.plugins.terminal.font:copy(
      config.plugins.terminal.font:get_size(), { smoothing = true }
    )
  elseif prev_scale ~= SCALE then
    config.plugins.terminal.font:set_size(
      (config.plugins.terminal.font:get_size() / prev_scale) * SCALE
    )
    config.plugins.terminal.bold_font = config.plugins.terminal.font:copy(
      config.plugins.terminal.font:get_size(), { smoothing = true }
    )
  end
end)

-- contrast functions pulled from https://github.com/xtermjs/xterm.js/blob/99df13b085aecb051f1373c5b7f8e819c4f41442/src/common/Color.ts#L285.
local function contrastRatio(l1, l2)
  if l1 < l2 then return (l2 + 0.05) / (l1 + 0.05) end
  return (l1 + 0.05) / (l2 + 0.05)
end

local function is_valid_color(color)
  if type(color) ~= "table" then return false end
  for i = 1, 3 do
    local channel = color[i]
    if type(channel) ~= "number" or channel ~= channel
      or channel < 0 or channel > 255 then
      return false
    end
  end
  return true
end

local function relativeLuminance(color)
  local rs = color[1] / 255
  local gs = color[2] / 255
  local bs = color[3] / 255
  local rr = rs <= 0.03928 and rs / 12.92 or (((rs + 0.055) / 1.055) ^ 2.4)
  local rg = gs <= 0.03928 and gs / 12.92 or (((gs + 0.055) / 1.055) ^ 2.4)
  local rb = bs <= 0.03928 and bs / 12.92 or (((bs + 0.055) / 1.055) ^ 2.4)
  return rr * 0.2126 + rg * 0.7152 + rb * 0.0722
end

local function reduceLuminance(bg, fg, ratio)
  local bgL = relativeLuminance(bg)
  local cr = contrastRatio(relativeLuminance(fg), bgL)
  local nFg = { table.unpack(fg) }
  while cr < ratio and (nFg[1] > 0 or nFg[2] > 0 or nFg[3] > 0) do
    -- Reduce by 10% until the ratio is hit
    nFg[1] = nFg[1] - math.max(0, math.ceil(nFg[1] * 0.1))
    nFg[2] = nFg[2] - math.max(0, math.ceil(nFg[2] * 0.1))
    nFg[3] = nFg[3] - math.max(0, math.ceil(nFg[3] * 0.1))
    cr = contrastRatio(relativeLuminance(nFg), bgL)
  end
  return nFg
end

local function increaseLuminance(bg, fg, ratio)
  local bgL = relativeLuminance(bg)
  local cr = contrastRatio(relativeLuminance(fg), bgL)
  local nFg = { table.unpack(fg) }
  local cr = contrastRatio(relativeLuminance(nFg), bgL)
  while cr < ratio and (nFg[1] < 0xFF or nFg[2] < 0xFF or nFg[3] < 0xFF) do
    -- Increase by 10% until the ratio is hit
    nFg[1] = math.min(0xFF, nFg[1] + math.ceil((255 - nFg[1]) * 0.1))
    nFg[2] = math.min(0xFF, nFg[2] + math.ceil((255 - nFg[2]) * 0.1))
    nFg[3] = math.min(0xFF, nFg[3] + math.ceil((255 - nFg[3]) * 0.1))
    cr = contrastRatio(relativeLuminance(nFg), bgL)
  end
  return nFg
end

local function ensureContrastRatio(bg, fg, ratio)
  local bgL = relativeLuminance(bg)
  local fgL = relativeLuminance(fg)
  local cr = contrastRatio(bgL, fgL)
  if cr < ratio then
    if fgL < bgL then
      local resultA = reduceLuminance(bg, fg, ratio)
      local resultARatio = contrastRatio(bgL, relativeLuminance(resultA))
      if resultARatio < ratio then
        local resultB = increaseLuminance(bg, fg, ratio)
        local resultBRatio = contrastRatio(bgL, relativeLuminance(resultB))
        return resultARatio > resultBRatio and resultA or resultB
      end
      return resultA
    end
    local resultA = increaseLuminance(bg, fg, ratio)
    local resultARatio = contrastRatio(bgL, relativeLuminance(resultA))
    if resultARatio < ratio then
      local resultB = reduceLuminance(bg, fg, ratio)
      local resultBRatio = contrastRatio(bgL, relativeLuminance(resultB))
      return resultARatio > resultBRatio and resultA or resultB
    end
    return resultA
  end
  return fg
end



local function text_length(text)
  return (string.ulen and string.ulen(text)) or #text
end

local function text_lower(text)
  return (string.ulower and string.ulower(text)) or text:lower()
end

local function line_plain_text(line)
  local text = {}
  for i = 2, #line, 2 do
    text[#text + 1] = line[i]
  end
  text = table.concat(text)
  if text:sub(-1) == "\n" then text = text:sub(1, -2) end
  return text
end

local function synchronized_output_enabled(emulator)
  return emulator and emulator.synchronized_output
    and emulator:synchronized_output() or false
end

local trailing_url_characters = {
  ["."] = true, [","] = true, [";"] = true, [":"] = true,
  ["!"] = true, ["?"] = true, [")"] = true, ["]"] = true,
  ["}"] = true
}

local function trim_url(url)
  while trailing_url_characters[url:sub(-1)] do
    url = url:sub(1, -2)
  end
  return url
end

local TerminalView = View:extend()
local search_view

function TerminalView:get_name()
  local name = self.terminal and self.terminal:name()
    or (self.session and self.session:id()) or "Terminal"
  return (self.modified_since_last_focus and "* " or "") .. name
end
function TerminalView:supports_text_input() return true end

function TerminalView:new(options)
  TerminalView.super.new(self)
  local view_options = type(options) == "table" and options or {}
  options = common.merge(common.merge({}, config.plugins.terminal), view_options)
  if type(view_options.colors) == "table" then
    options.colors = common.merge(config.plugins.terminal.colors, view_options.colors)
  end
  self.size.y = options.drawer_height
  self.options = options
  self.session = options.session
  self.emulator = options.emulator
  self.terminal = self.emulator
  self.cursor = "ibeam"
  self.scrollable = true
  self.last_size = { x = self.size.x, y = self.size.y }
  self.focused = false
  self.modified_since_last_focus = false
  self.mouse_buttons = {}
  self.session_attached = false
  self.search_state = {
    text = "",
    case_sensitive = config.find_case_sensitive or false,
    matches = {},
    index = 0,
    match = nil
  }
end

---Invalidate text-derived terminal data after output or geometry changes.
function TerminalView:invalidate_output_cache()
  self.output_cache = nil
  self.links_cache = nil
end


---Return terminal output with stable logical rows, including scrollback.
---@return table[]
function TerminalView:get_output_lines()
  if not self.terminal then return {} end
  local scrollback, total_scrollback = self.terminal:scrollback()
  local columns, rows = self.terminal:size()
  local cache = self.output_cache
  if cache and cache.scrollback == scrollback
      and cache.total_scrollback == total_scrollback
      and cache.columns == columns and cache.rows == rows then
    return cache.lines
  end
  local first_row = -total_scrollback
  local result = {}
  for index, line in ipairs(self.terminal:lines(first_row, rows - 1)) do
    result[#result + 1] = {
      row = first_row + index - 1,
      text = line_plain_text(line)
    }
  end
  self.output_cache = {
    lines = result,
    scrollback = scrollback,
    total_scrollback = total_scrollback,
    columns = columns,
    rows = rows
  }
  self.links_cache = nil
  return result
end

---Find plain-text matches in the complete terminal buffer.
---@param query string
---@param case_sensitive boolean?
---@return table[]
function TerminalView:find_matches(query, case_sensitive)
  if not self.terminal or query == nil or query == "" then return {} end
  case_sensitive = case_sensitive == nil and self.search_state.case_sensitive or case_sensitive
  local needle = case_sensitive and query or text_lower(query)
  local matches = {}
  for _, line in ipairs(self:get_output_lines()) do
    local haystack = case_sensitive and line.text or text_lower(line.text)
    local offset = 1
    while true do
      local start, finish = haystack:find(needle, offset, true)
      if not start then break end
      local prefix = line.text:sub(1, start - 1)
      local match_text = line.text:sub(start, finish)
      local col1 = text_length(prefix)
      matches[#matches + 1] = {
        row = line.row,
        col1 = col1,
        col2 = col1 + text_length(match_text),
        text = match_text
      }
      offset = finish + 1
    end
  end
  return matches
end

local function same_match(left, right)
  return left and right
    and left.row == right.row
    and left.col1 == right.col1
    and left.col2 == right.col2
end

function TerminalView:refresh_search(preserve_match)
  local state = self.search_state
  local old_match = preserve_match and state.match
  state.matches = self:find_matches(state.text, state.case_sensitive)
  state.index = 0
  if old_match then
    for index, match in ipairs(state.matches) do
      if same_match(match, old_match) then
        state.index = index
        break
      end
    end
  end
  if state.index == 0 and #state.matches > 0 then state.index = 1 end
  state.match = state.matches[state.index]
  self.search_matches = state.matches
  self.active_search_match = state.match
  core.redraw = true
  return state.matches
end

function TerminalView:set_search_text(text)
  self.search_state.text = text or ""
  local matches = self:refresh_search(false)
  if self.search_state.match then self:reveal_search_match() end
  return matches
end

function TerminalView:reveal_search_match()
  local match = self.search_state.match
  if not match or not self.terminal then return end
  local _, total_scrollback = self.terminal:scrollback()
  local target_scrollback = math.min(total_scrollback, math.max(0, -match.row))
  self.terminal:scrollback(target_scrollback)
end

function TerminalView:search_next(reverse)
  local state = self.search_state
  if state.text == "" then return nil end
  if #state.matches == 0 then
    self:refresh_search(false)
  end
  if #state.matches == 0 then return nil end
  local step = reverse and -1 or 1
  state.index = ((state.index - 1 + step) % #state.matches) + 1
  state.match = state.matches[state.index]
  self.active_search_match = state.match
  self:reveal_search_match()
  core.redraw = true
  return state.match
end

function TerminalView:toggle_search_case_sensitive()
  self.search_state.case_sensitive = not self.search_state.case_sensitive
  self:refresh_search(true)
  self:reveal_search_match()
  return self.search_state.case_sensitive
end

function TerminalView:get_links()
  local output_cache = self.output_cache
  if self.links_cache and self.links_cache.output_cache == output_cache then
    return self.links_cache.links
  end
  local links = {}
  for _, line in ipairs(self:get_output_lines()) do
    for start, raw_url in line.text:gmatch("()([%a][%w+.-]*://[^%s<>]+)") do
      local url = trim_url(raw_url)
      if url ~= "" then
        local prefix = line.text:sub(1, start - 1)
        links[#links + 1] = {
          url = url,
          row = line.row,
          col1 = text_length(prefix),
          col2 = text_length(prefix) + text_length(url)
        }
      end
    end
  end
  self.links_cache = { output_cache = self.output_cache, links = links }
  return links
end

function TerminalView:get_link_at(x, y)
  if not self.terminal then return nil end
  local col, screen_row = self:convert_coordinates(x, y)
  local scrollback = self.terminal:scrollback()
  local row = screen_row - scrollback
  local links = self:get_links()
  for _, link in ipairs(links) do
    if link.row == row and col >= link.col1 and col < link.col2 then
      return link
    end
  end
end

function TerminalView:set_hovered_link(link)
  local url = link and link.url
  if self.hovered_link_url == url
    and (not link or not self.hovered_link
      or (self.hovered_link.row == link.row and self.hovered_link.col1 == link.col1))
  then
    return
  end
  self.hovered_link = link
  self.hovered_link_url = url
  if url then
    core.status_view:show_tooltip("Open " .. url)
  else
    core.status_view:remove_tooltip()
  end
  core.redraw = true
end

function TerminalView:open_link(url)
  if url then common.open_in_system(url) end
end

function TerminalView:open_link_at(x, y)
  local link = self:get_link_at(x, y)
  if link then
    self:open_link(link.url)
    return link
  end
end


function TerminalView:attach_session()
  if not self.session or self.session_attached then return true end
  if self.session.attach then
    local result, message = self.session:attach()
    if result == false then
      self.session_error = message
      return false
    end
  end
  self.session_attached = true
  self.session_error = nil
  return true
end


function TerminalView:shift_selection_update()
  local shifts = 0
  local content_changed = false
  if self.session then
    if not self:attach_session() then return 0 end
    local events = self.session:poll_events()
    local capabilities = self.session.capabilities or {}
    for _, event in ipairs(events) do
      if event.type == "output" then
        if not capabilities.events_applied then
          shifts = shifts + (self.emulator:feed(event.data) or 0)
        end
        if not synchronized_output_enabled(self.emulator) then
          content_changed = true
        end
      elseif event.type == "checkpoint" then
        content_changed = true
        local emulator, message
        if self.session.apply_checkpoint then
          emulator, message = self.session:apply_checkpoint(event, self.emulator)
        else
          emulator, message = false, "session does not support checkpoints"
        end
        if emulator then
          self.emulator = emulator
          self.terminal = emulator
          self.checkpoint_error = nil
        else
          self.checkpoint_error = message
        end
      elseif event.type == "gap" then
        self.replay_event = event
      elseif event.type == "status" and event.status == "exited" then
        self.exit_event = event
      end
    end
    shifts = shifts + (self.session.last_shifts or 0)
  end
  if content_changed then
    self:invalidate_output_cache()
    if self.search_state.text ~= "" then
      self:refresh_search(true)
    end
  end
  if shifts and not self.focused then self.modified_since_last_focus = true end
  if self.selection and shifts then
    self.selection[2] = self.selection[2] - shifts
    self.selection[4] = self.selection[4] - shifts
    if math.abs(math.min(self.selection[2], self.selection[4])) > self.options.scrollback_limit then
      self.selection = nil
    end
  end
  return synchronized_output_enabled(self.emulator) and 0 or shifts
end


function TerminalView:start_background()
  if self.routine then return end
  self.background_generation = (self.background_generation or 0) + 1
  local generation = self.background_generation
  -- We make this weak so that any other method of closing the view gets caught up in the garbage collection and the coroutine doesn't count as a reference for gc purposes.
  local weak_table = { self = self }
  setmetatable(weak_table, { __mode = "v" })
  self.routine = core.add_background_thread(function()
    local count = 0
    while weak_table.self and weak_table.self.session
      and weak_table.self.background_generation == generation do
      -- do not redraw when hidden
      if weak_table.self.size.y > 0 then
        local shifts = weak_table.self:shift_selection_update()
        core.redraw = shifts or core.redraw
        if core.active_view == self and not core.redraw
            and not synchronized_output_enabled(weak_table.self.emulator) then
          core.redraw = true
          coroutine.yield(0.5)
        else
          coroutine.yield(1/30)
        end
      else
        if weak_table.self:shift_selection_update() then
          weak_table.self:update()
        end
        coroutine.yield(1)
      end
      -- make sure to exit if no longer part of the root view
      if count >= 500 then
        local found = false
        local views = core.root_view.root_node:get_children()
        for _, view in ipairs(views) do
          if view == weak_table.self then found = true break end
        end
        if not found then break end
        count = 0
      else
        count = count + 1
      end
    end
  end)
end

function TerminalView:create_session()
  local project = core.root_project()
  return LocalSession {
    id = self.options.id,
    cwd = (project and project.path) or system.getcwd(),
    columns = self.columns,
    rows = self.lines,
    command = self.options.shell,
    args = self.options.arguments,
    environment = self.options.environment,
    term = self.options.term,
    scrollback_limit = self.options.scrollback_limit,
    debug = self.options.debug,
    terminate_on_detach = self.options.terminate_on_detach
  }
end

function TerminalView:spawn()
  if not self.session then self.session = self:create_session() end
  self.emulator = self.emulator or self.session.emulator or Emulator {
    columns = self.columns, rows = self.lines,
    scrollback_limit = self.options.scrollback_limit,
    term = self.options.term, environment = self.options.environment,
    debug = self.options.debug
  }
  self.terminal = self.emulator
  self:invalidate_output_cache()
  self:attach_session()
  self:start_background()
end

function TerminalView:detach_session()
  local session = self.session
  if session then session:detach() end
  self.background_generation = (self.background_generation or 0) + 1
  self.session = nil
  self.session_attached = false
  self.emulator = nil
  self.terminal = nil
  self:invalidate_output_cache()
  self.routine = nil
  return session
end


---Can be overriden by widgets to listen for scale change events to apply
---any neccesary changes in sizes, padding, etc...
---@param new_scale number
---@param prev_scale number
function TerminalView:on_scale_change(new_scale, prev_scale)
  TerminalView.super.on_scale_change(self, new_scale, prev_scale)
  if self.options.font ~= style.code_font then
    self.options.font:set_size(
      self.options.font:get_size() * (new_scale / prev_scale)
    )
  end
end


function TerminalView:update()
  TerminalView.super.update(self)
  if self.last_font_size and self.last_font_size ~= self.options.font:get_size() then
    self.options.bold_font:set_size(self.options.font:get_size())
  end
  if (self.last_font_size and self.last_font_size ~= self.options.font:get_size()) or self.size.x > 0 and self.size.y > 0 and not self.terminal or self.last_size.x ~= self.size.x or self.last_size.y ~= self.size.y then
    self.columns = math.max(math.floor((self.size.x - self.options.padding.x*2) / self.options.font:get_width("W")), 1)
    self.lines = math.max(math.floor((self.size.y - self.options.padding.y*2) / self.options.font:get_height()), 1)
    if self.lines > 0 and self.columns > 0 then
      if not self.terminal then
        self:spawn()
      else
        self.session:resize(self.columns, self.lines)
        if self.emulator ~= self.session.emulator then
          self.emulator:resize(self.columns, self.lines)
        end
        self:invalidate_output_cache()
        self.last_size = { x = self.size.x, y = self.size.y }
      end
    end
  end
  if self.terminal then
    core.redraw = self:shift_selection_update() or core.redraw
    if self.deferred_input then
      self:input(self.deferred_input)
      self.deferred_input = nil
    end
    local status = self.session:status()
    if status ~= "exited" and status ~= "closed" and status ~= "error" then
      self.cursor = "ibeam"
      if self.terminal:mouse_tracking_mode()
         or self.v_scrollbar.hovering.track
         or self.hovered_link_url then
        self.cursor = "arrow"
      end
      if self.hovered_link_url then self.cursor = "hand" end
      if (core.active_view == self and not self.focused) or (core.active_view ~= self and self.focused) then
        self.focused = core.active_view == self
        self.modified_since_last_focus = false
        self.emulator:focused(self.focused)
      end
      local x, y, mode = self.terminal:cursor()
      if mode == "blinking" then
        local T, t0 = config.blink_period, core.blink_start
        local ta, tb = core.blink_timer, system.get_time()
        if ((tb - t0) % T < T / 2) ~= ((ta - t0) % T < T / 2) then
          core.redraw = true
        end
        core.blink_timer = tb
      end
      if self.scrolling_offscreen and (not self.last_scroll_time or system.get_time() - self.last_scroll_time > self.options.scrolling_speed) then
        self.last_scroll_time = system.get_time()
        self.terminal:scrollback(self.terminal:scrollback() + self.scrolling_offscreen)
        core.redraw = true
      end
      local scrollback, total_scrollback = self.terminal:scrollback()
      local lh = self.options.font:get_height()
      -- 0 should be if we're at max scrollback. 1 should be if we're at 0 scrollback.
      self.v_scrollbar:set_size(self.position.x, self.position.y, self.size.x, self.size.y, self:get_scrollable_size())
      self.v_scrollbar:set_percent(1.0 - (scrollback / total_scrollback))
      self.v_scrollbar:update()
    else
      self.cursor = "arrow"
      core.redraw = true
    end
  end
  self.last_font_size = self.options.font:get_size()
end


function TerminalView:set_target_size(axis, value)
  if axis == "y" then
    self.size.y = value
    return true
  end
end

function TerminalView:convert_color(int, target, should_bright)
  local fallback = target == "foreground" and self.options.text or self.options.background
  if not is_valid_color(fallback) then
    fallback = target == "foreground"
      and { 255, 255, 255, 255 }
      or { 0, 0, 0, 255 }
  end

  local attributes = bit.rshift(int, 24)
  local type = bit.band(attributes, 0x7)
  if type == 0 then
    local color = target == "foreground" and self.options.text or self.options.background
    return is_valid_color(color) and color or fallback, attributes
  elseif type == 1 then
    local color = target == "foreground" and self.options.background or self.options.text
    return is_valid_color(color) and color or fallback, attributes
  elseif type == 2 then
    local index = bit.band(bit.rshift(int, 16), 0xFF)
    if index < 8 and should_bright and (bit.band(bit.rshift(attributes, 3), 0x1) ~= 0) then index = index + 8 end
    local color = self.options.colors and self.options.colors[tonumber(index)]
    return is_valid_color(color) and color or fallback, attributes
  elseif type == 3 then
    return {
      tonumber(bit.band(bit.rshift(int, 16), 0xFF)),
      tonumber(bit.band(bit.rshift(int, 8), 0xFF)),
      tonumber(bit.band(bit.rshift(int, 0), 0xFF)),
      255
    }, attributes
  end
  return fallback, attributes
end

function TerminalView:sorted_selection()
  if not self.selection then return nil end
  local selection = { table.unpack(self.selection) }
  if selection[2] > selection[4] or (selection[2] == selection[4] and selection[1] > selection[3]) then
    selection = { selection[3], selection[4], selection[1], selection[2] }
  end
  return selection
end


local contrast_foreground = {}

local function apply_text_range(sections, range_start, range_end, background,
    foreground, subfunc, lengthfunc)
  local result = {}
  local offset = 0
  for _, section in ipairs(sections) do
    local text = section[3]
    local length = lengthfunc(text)
    local start = math.max(range_start, offset)
    local finish = math.min(range_end, offset + length)
    if start < finish then
      if start > offset then
        result[#result + 1] = {
          section[1], section[2], subfunc(text, 1, start - offset)
        }
      end
      result[#result + 1] = {
        background, foreground,
        subfunc(text, start - offset + 1, finish - offset)
      }
      if finish < offset + length then
        result[#result + 1] = {
          section[1], section[2], subfunc(text, finish - offset + 1)
        }
      end
    else
      result[#result + 1] = section
    end
    offset = offset + length
  end
  return result
end

function TerminalView:draw()
  TerminalView.super.draw_background(self, self.options.background)
  if self.terminal then
    local cursor_x, cursor_y, mode = self.terminal:cursor()
    local space_width = self.options.font:get_width(" ")

    local y = self.position.y + self.options.padding.y
    local lh = self.options.font:get_height()


    local selection = self:sorted_selection()
    for line_idx, line in ipairs(self.terminal:lines()) do
      local x = self.position.x + self.options.padding.x
      local should_draw_cursor = false
      if mode ~= "hidden" and core.active_view == self and line_idx - 1 == cursor_y and self.terminal:scrollback() == 0 then
        if mode == "blinking" then
          local T = config.blink_period
          if (core.blink_timer - core.blink_start) % T < T / 2 then
            should_draw_cursor = true
          end
        else
          should_draw_cursor = true
        end
      end
      local offset = 0
      local foreground, background, text_style
      for i = 1, #line, 2 do
        line[i] = math.tointeger(line[i])
        background = self:convert_color(bit.band(line[i], 0xFFFFFFFF), "background")
        foreground, text_style = self:convert_color(bit.rshift(line[i], 32), "foreground", self.options.bold_text_in_bright_colors)

        if config.plugins.terminal.minimum_contrast_ratio > 0 then
          if not contrast_foreground[line[i]] then
            contrast_foreground[line[i]] = ensureContrastRatio(background, foreground, config.plugins.terminal.minimum_contrast_ratio)
          end
          foreground = contrast_foreground[line[i]]
        end

        local font = (bit.band(bit.rshift(text_style, 3), 0x1) ~= 0) and self.options.bold_font or self.options.font
        local text = line[i+1]
        local length = text:ulen()
        local valid_utf8 = length ~= nil
        local subfunc, lengthfunc = string.usub, string.ulen
        if not valid_utf8 then
          length = #text
          lengthfunc = function(str) return #str end
          subfunc = string.sub
        end
        local idx = (line_idx - 1) - self.terminal:scrollback()
        local sections = { { background, foreground, text } }
        if selection then
          if ((idx == selection[2] and selection[1] <= offset) or selection[2] < idx) and (selection[4] > idx or (idx == selection[4] and (selection[3] >= offset + length))) then -- overlaps all
            sections = { { foreground, background, text } }
          elseif (idx == selection[2] and idx == selection[4] and selection[1] > offset and selection[3] < offset + length) then -- overlaps in middle
            sections = { { background, foreground, subfunc(text, 1, selection[1] - offset) }, { foreground, background, subfunc(text, selection[1] - offset + 1, selection[3] - offset) }, { background, foreground, subfunc(text, selection[3] - offset + 1, length) } }
          elseif (selection[2] < idx or (idx == selection[2] and selection[1] <= offset)) and (idx == selection[4] and selection[3] < offset + length and selection[3] >= offset) then -- overlaps start
            sections = { { foreground, background, subfunc(text, 1, selection[3] - offset) }, { background, foreground, subfunc(text, selection[3] - offset + 1, length) } }
          elseif (idx == selection[2] and selection[1] < offset + length and selection[1] >= offset) and (selection[4] > idx or (selection[4] == idx and selection[3] >= offset + length)) then -- overlaps end
            sections = { { background, foreground, subfunc(text, 1, selection[1] - offset) }, { foreground, background, subfunc(text, selection[1] - offset + 1, length) } }
          end
        end
        local active_match = self.active_search_match
        if active_match and active_match.row == idx then
          sections = apply_text_range(
            sections, active_match.col1 - offset, active_match.col2 - offset,
            style.search_selection or style.selection,
            style.search_selection_text or style.text,
            subfunc, lengthfunc
          )
        end
        local hovered_link = self.hovered_link
        if hovered_link and hovered_link.row == idx then
          sections = apply_text_range(
            sections, hovered_link.col1 - offset, hovered_link.col2 - offset,
            style.line_highlight or style.selection, style.accent,
            subfunc, lengthfunc
          )
        end
        -- split sections further, to insert an inverted bit for the cursor
        if should_draw_cursor and cursor_x >= offset and cursor_x < offset + length then
          local local_offset = offset
          for i,v in ipairs(sections) do
            local len = lengthfunc(v[3])
            if cursor_x >= local_offset and cursor_x < local_offset + len then
              sections[i] = nil
              local target = i
              if cursor_x ~= local_offset then
                table.insert(sections, target, { v[1], v[2], subfunc(v[3], 1, cursor_x - local_offset) })
                target = target + 1
              end
              table.insert(sections, target, { v[2], v[1], subfunc(v[3], cursor_x - local_offset + 1, cursor_x - local_offset + 1) })
              target = target + 1
              if cursor_x ~= local_offset + len - 1 then
                table.insert(sections, target, { v[1], v[2], subfunc(v[3], cursor_x - local_offset + 2) })
              end
              break
            end
            local_offset = local_offset + len
          end
        end
        for i, section in ipairs(sections) do
          if section then
            local background, foreground, text = table.unpack(section)
            if background and background ~= self.options.background then
              renderer.draw_rect(x, y, (text:ulen() or #text)*space_width, lh, background)
            end
            x = renderer.draw_text(font, text, x, y, foreground)
          end
        end
        offset = offset + length
      end
      y = y + lh
    end
  end
  TerminalView.super.draw_scrollbar(self)
end

function TerminalView:convert_coordinates(x, y)
  local w = self.options.font:get_width(" ")
  local col_exact = math.floor((x - self.position.x - self.options.padding.x) / w)
  local col_approx = common.round((x - self.position.x - self.options.padding.x) / w)
  local row = math.floor((y - self.position.y - self.options.padding.y) / self.options.font:get_height())
  return math.max(0, col_exact), math.max(0, row), math.max(0, col_approx)
end

function TerminalView:get_mouse_tracking()
  if not self.terminal  then return end
  local mode = self.terminal:mouse_tracking_mode()
  if not mode then return nil end
  return mode, self.terminal:mouse_encoding()
end

function TerminalView:get_mouse_button_code(button)
  if button == "left" then return 0 end
  if button == "middle" then return 1 end
  if button == "right" then return 2 end
end

function TerminalView:get_active_mouse_button_code()
  for _, button in ipairs({ "left", "middle", "right" }) do
    if self.mouse_buttons[button] then
      return self:get_mouse_button_code(button)
    end
  end
  return 3
end

function TerminalView:send_mouse_event(button_code, col, row, suffix, encoding, event)
  local native_event = event == "moved" and 4
    or (event == "released" and 2 or 1)
  if self.terminal.mouse and self.terminal:mouse(
      col, row, button_code, native_event, 0) then
    return true
  end
  if encoding == "sgr" then
    self.terminal:input("\x1B[<" .. button_code .. ";" .. (col + 1) .. ";" .. (row + 1) .. suffix)
  else
    self.terminal:input("\x1B[M" .. string.char(32 + button_code) .. string.char(32 + col + 1) .. string.char(32 + row + 1))
  end
  return false
end

function TerminalView:get_word_boundaries(col, row)
  for line_idx, line in ipairs(self.terminal:lines()) do
    if line_idx == row + 1 then
      local text = ""
      for i = 1, #line, 2 do
        text = text .. line[i+1]
      end
      local scrollback = self.terminal:scrollback()
      row = row - scrollback
      if text:sub(col + 1, col + 1):match("%s") then
        return col, row, col + 1, row
      end
      local next_space = text:find("%s", col + 1) or #text
      local last_space = 0
      local idx = text:reverse():find("%s", #text - col)
      if idx then
        last_space = (#text - idx) + 1
      end
      return last_space, row, next_space - 1, row
    end
  end
end

function TerminalView:on_mouse_pressed(button, x, y, clicks)
  local result = self.v_scrollbar:on_mouse_pressed(button, x, y, clicks)
  if result then
    if result ~= true then
      local _, total_scrollback = self.terminal:scrollback()
      self.terminal:scrollback(math.floor((1.0 - result) * total_scrollback))
    end
    return true
  end
  self.mouse_x = x
  self.mouse_y = y
  local col, row = self:convert_coordinates(x, y)
  local inverted = config.plugins.terminal.inversion_key and keymap.modkeys[config.plugins.terminal.inversion_key]
  local mouse_mode, mouse_encoding = self:get_mouse_tracking()
  local button_code = self:get_mouse_button_code(button)
  if not inverted and mouse_mode and button_code ~= nil then
    self.mouse_buttons[button] = true
    self:send_mouse_event(button_code, col, row, "M", mouse_encoding, "pressed")
    return true
  end
  if button == "left" and self:open_link_at(x, y) then
    self.selection = nil
    return true
  end
  if button == "left" then
    if clicks % 4 == 1 then
      self.selection = nil
      self.pressing = true
    elseif clicks % 4 == 2 then
      self.word_selecting = { self:get_word_boundaries(col, row) }
      self.selection = { table.unpack(self.word_selecting) }
    elseif clicks % 4 == 3 then
      local scrollback = self.terminal:scrollback()
      row = row - scrollback
      self.row_selecting = { 0, row, 0, row + 1 }
      self.selection = { 0, row, 0, row + 1 }
    end
  end
end

function TerminalView:on_mouse_moved(x, y, dx, dy)
  local result = self.v_scrollbar:on_mouse_moved(x, y, dx, dy)
  if result then
    if result ~= true then
      local _, total_scrollback = self.terminal:scrollback()
      self.terminal:scrollback(math.floor((1.0 - result) * total_scrollback))
    end
    return true
  end
  self.mouse_x = x
  self.mouse_y = y
  local inverted = config.plugins.terminal.inversion_key and keymap.modkeys[config.plugins.terminal.inversion_key]
  local mouse_mode, mouse_encoding = self:get_mouse_tracking()
  if not inverted and mouse_mode then
    local button_code = self:get_active_mouse_button_code()
    if mouse_mode == "any" or (mouse_mode == "button" and button_code ~= 3) then
      local col, row = self:convert_coordinates(x, y)
      self:send_mouse_event(button_code + 32, col, row, "M", mouse_encoding, "moved")
      return true
    end
  end
  if not self.pressing and not self.word_selecting and not self.row_selecting then
    self:set_hovered_link(self:get_link_at(x, y))
  end
  if self.pressing or self.word_selecting or self.row_selecting then
    if y < self.position.y then
      self.scrolling_offscreen = 1
    elseif y > self.position.y + self.size.y then
      self.scrolling_offscreen = -1
    else
      self.scrolling_offscreen = nil
    end
    local col, line, col_approx = self:convert_coordinates(x, y)
    local scrollback = self.terminal:scrollback()
    if not self.selection then self.selection = { col_approx, line - scrollback } end
    if not self.word_selecting and not self.row_selecting then
      self.selection[3] = col_approx
      self.selection[4] = line - scrollback
    elseif self.word_selecting then
      local col1, line1, col2, line2 = self:get_word_boundaries(col, line)
      if not col1 then
        self.selection[3] = col
        self.selection[4] = line - scrollback
      elseif self.word_selecting[2] > line1 or (self.word_selecting[2] == line1 and self.word_selecting[1] >= col1) then
        self.selection[1] = col1
        self.selection[2] = line1
        self.selection[3] = self.word_selecting[3]
        self.selection[4] = self.word_selecting[4]
      else
        self.selection[1] = self.word_selecting[1]
        self.selection[2] = self.word_selecting[2]
        self.selection[3] = col2
        self.selection[4] = line2
      end
    else
      self.selection[1] = 0
      self.selection[2] = math.min(self.row_selecting[2], line - scrollback)
      self.selection[3] = 0
      self.selection[4] = math.max(self.row_selecting[4], line - scrollback + 1)
    end
  end
end


function TerminalView:on_mouse_released(button, x, y)
  self.v_scrollbar:on_mouse_released(button, x, y)
  self.mouse_x = x
  self.mouse_y = y
  local col, row = self:convert_coordinates(x, y)
  local inverted = config.plugins.terminal.inversion_key and keymap.modkeys[config.plugins.terminal.inversion_key]
  local mouse_mode, mouse_encoding = self:get_mouse_tracking()
  local button_code = self:get_mouse_button_code(button)
  if button_code ~= nil and not inverted and mouse_mode and mouse_mode ~= "x10" then
    self:send_mouse_event(3, col, row, "m", mouse_encoding, "released")
  end
  if button_code ~= nil then
    self.mouse_buttons[button] = nil
  end
  if button == "left" then
    self.pressing = false
    self.word_selecting = nil
    self.row_selecting = nil
    self.scrolling_offscreen = nil
  end
end

function TerminalView:on_mouse_left()
  TerminalView.super.on_mouse_left(self)
  self:set_hovered_link(nil)
  self.cursor = "ibeam"
end


function TerminalView:get_scrollable_size()
  if not self.terminal then return self.size.y end
  local lh = self.options.font:get_height()
  local _, total_scrollback = self.terminal:scrollback()
  return total_scrollback * lh + self.size.y
end


function TerminalView:input(text)
  if self.session and self.emulator then
    self.session:write(text)
    if self.terminal:scrollback() ~= 0 then self.terminal:scrollback(0) end
    self:shift_selection_update()
    core.redraw = true
    return true
  else
    self.deferred_input = (self.deferred_input or "") .. text
  end
  return false
end

local function terminal_keyboard_modifiers()
  local modifiers = 0
  if keymap.modkeys.shift then modifiers = modifiers + 1 end
  if keymap.modkeys.ctrl then modifiers = modifiers + 4 end
  if keymap.modkeys.alt or keymap.modkeys.altgr or keymap.modkeys.option then
    modifiers = modifiers + 8
  end
  if keymap.modkeys.super or keymap.modkeys.cmd then modifiers = modifiers + 16 end
  return modifiers
end

function TerminalView:keyboard(key_name, unicode)
  if self.terminal and self.terminal.keyboard then
    local handled = self.terminal:keyboard(key_name,
      terminal_keyboard_modifiers(), unicode)
    if handled then
      if self.terminal:scrollback() ~= 0 then self.terminal:scrollback(0) end
      self:shift_selection_update()
      core.redraw = true
    end
    return handled
  end
  return false
end

function TerminalView:on_text_input(text)
  return self:input(text)
end

function TerminalView:restart()
  if self.session then self.session:terminate({ restart = true }) end
  self.session = nil
  self.emulator = nil
  self.terminal = nil
  self:invalidate_output_cache()
  self.routine = nil
  self:update()
end

function TerminalView:close()
  if self.session then self:detach_session() end
  local node = core.root_view.root_node:get_node_for_view(self)
  if node then node:close_view(core.root_view.root_node, self) end
  if core.terminal_view == self then core.terminal_view = nil end
end


function TerminalView:get_text(line1, col1, line2, col2)
  local full_buffer = {}
  for line_idx, line in ipairs(self.terminal:lines(line1, line2)) do
    local idx = line_idx - 1 + line1
    local offset = 0
    for i = 1, #line, 2 do
      local text = line[i + 1]
      local length = text:ulen()
      local usub = string.usub
      if length == nil then
        length = #text
        usub = string.sub
      end
      if idx == line1 and idx == line2 then
        if offset + length >= col1 and offset <= col2 then
          local s = math.max(col1 - offset, 0) + 1
          local e = math.min(col2 - offset, length)
          table.insert(full_buffer, usub(text, s, e))
        end
      elseif idx == line1 then
        if offset + length >= col1 then
          local s = math.max(col1 - offset, 0) + 1
          table.insert(full_buffer, usub(text, s, length))
        end
      elseif idx > line1 and idx < line2 then
        table.insert(full_buffer, text)
      elseif idx == line2 then
        if offset <= col2 then
          local e = math.min(col2 - offset, length)
          table.insert(full_buffer, usub(text, 1, e))
        end
      end
      offset = offset + length
    end
  end
  return table.concat(full_buffer)
end

local function adjust_font_size(view, delta)
  local size = math.max(view.options.font:get_size() + delta, 1)
  view.options.font = view.options.font:copy(size)
  view.options.bold_font = view.options.font:copy(size, { smoothing = true })
  view:update()
end

local function is_terminal_view(view)
  return view and view:extends(TerminalView)
end

command.add(function(amount)
  -- core.root_view.overlapping_view is not in any release yet (as of 2.1.3)
  local view = core.root_view.overlapping_view
                or (core.root_view.overlapping_node and core.root_view.overlapping_node.active_view)
                or core.active_view
  return (is_terminal_view(view) and view.terminal), view, amount
end, {
  ["terminal:scroll"] = function(view, amount)
    if not view.terminal then return end
    local inverted = config.plugins.terminal.inversion_key and keymap.modkeys[config.plugins.terminal.inversion_key]
    local mouse_mode, mouse_encoding = view:get_mouse_tracking()
    if not inverted and mouse_mode and mouse_mode ~= "x10" then
      local col, row = view:convert_coordinates(view.mouse_x or view.position.x, view.mouse_y or view.position.y)
      view:send_mouse_event(amount > 0 and 64 or 65, col, row, "M", mouse_encoding, "pressed")
    else
      if not inverted and not mouse_mode and view.terminal.mouse then
        local col, row = view:convert_coordinates(view.mouse_x or view.position.x, view.mouse_y or view.position.y)
        if view.terminal:mouse(col, row, amount > 0 and 64 or 65, 1, 0) then
          return
        end
      end
      view.accumulated_scroll = (view.accumulated_scroll or 0) + (amount or 1)
      if math.abs(view.accumulated_scroll) >= 1 then
        local delta = math.floor(view.accumulated_scroll + 0.5)
        view.terminal:scrollback(view.terminal:scrollback() + delta)
        view.accumulated_scroll = view.accumulated_scroll - math.floor(view.accumulated_scroll + 0.5)
      end
    end
  end
})


local active_terminal_predicate = function(...)
  return (is_terminal_view(core.active_view) and core.active_view.terminal), core.active_view, ...
end

local function terminal_search_predicate()
  local view
  if is_terminal_view(core.active_view) then
    view = core.active_view
  elseif search_view and core.active_view == core.command_view then
    view = search_view
  end
  local state = view and view.search_state
  local active = state and (state.text ~= "" or core.active_view == core.command_view)
  return active and view.terminal, view
end

local function terminal_search_tooltip(view)
  local state = view.search_state
  local next_binding = keymap.get_binding("terminal:search-next")
  local previous_binding = keymap.get_binding("terminal:search-previous")
  local case_binding = keymap.get_binding("terminal:search-toggle-case-sensitive")
  return (state.case_sensitive and "[Sensitive] " or "")
    .. (next_binding and ("Press " .. next_binding .. " for next. ") or "")
    .. (previous_binding and ("Press " .. previous_binding .. " for previous. ") or "")
    .. (case_binding and ("Press " .. case_binding .. " to toggle case.") or "")
end

local function open_terminal_search(view)
  local state = view.search_state
  local previous_text = state.text
  local previous_case_sensitive = state.case_sensitive
  search_view = view
  core.status_view:show_tooltip(terminal_search_tooltip(view))
  core.command_view:enter("Search Terminal", {
    text = state.text,
    select_text = true,
    show_suggestions = false,
    typeahead = false,
    suggest = function(text)
      view:set_search_text(text)
      core.status_view:show_tooltip(terminal_search_tooltip(view))
      return {}
    end,
    submit = function(text)
      view:set_search_text(text)
      search_view = view
      core.status_view:show_tooltip(terminal_search_tooltip(view))
    end,
    cancel = function()
      view.search_state.case_sensitive = previous_case_sensitive
      view:set_search_text(previous_text)
      search_view = view
      core.status_view:show_tooltip(terminal_search_tooltip(view))
    end
  })
end

command.add(active_terminal_predicate, {
  ["terminal:search"] = open_terminal_search
})

command.add(terminal_search_predicate, {
  ["terminal:search-next"] = function(view) view:search_next(false) end,
  ["terminal:search-previous"] = function(view) view:search_next(true) end,
  ["terminal:search-toggle-case-sensitive"] = function(view)
    view:toggle_search_case_sensitive()
    core.status_view:show_tooltip(terminal_search_tooltip(view))
  end
})

local function terminal_link_predicate()
  local view = core.active_view
  return is_terminal_view(view) and view.context_url ~= nil, view
end

command.add(terminal_link_predicate, {
  ["terminal:open-link"] = function(view)
    view:open_link(view.context_url)
  end,
  ["terminal:copy-link"] = function(view)
    system.set_clipboard(view.context_url)
  end
})

local contextmenu_available, terminal_contextmenu = pcall(require, "plugins.contextmenu")
if contextmenu_available and terminal_contextmenu then
  terminal_contextmenu:register(function(x, y)
    local view = core.active_view
    if not is_terminal_view(view) then return false end
    local link = view:get_link_at(x, y)
    view.context_url = link and link.url
    return link ~= nil
  end, {
    { text = "Open Link", command = "terminal:open-link" },
    { text = "Copy Link", command = "terminal:copy-link" }
  })
end

local function send_terminal_key(view, key_name, fallback)
  if not view:keyboard(key_name) then view:input(fallback) end
end

command.add(active_terminal_predicate, {
  ["terminal:backspace"] = function(view) send_terminal_key(view, "backspace", view.options.backspace) end,
  ["terminal:ctrl-backspace"] = function(view) send_terminal_key(view, "backspace", view.options.backspace == "\b" and "\x7F" or "\b") end,
  ["terminal:alt-backspace"] = function(view) send_terminal_key(view, "backspace", "\x1B" .. view.options.backspace) end,
  ["terminal:insert"] = function(view) send_terminal_key(view, "insert", "\x1B[2~") end,
  ["terminal:delete"] = function(view) send_terminal_key(view, "delete", view.options.delete) end,
  ["terminal:return"] = function(view) send_terminal_key(view, "return", view.options.newline) end,
  ["terminal:enter"] = function(view) send_terminal_key(view, "enter", view.terminal:cursor_keys_mode() == "application" and "\x1BOM" or view.options.newline) end,
  ["terminal:break"] = function(view) send_terminal_key(view, "c", "\x03") end,
  ["terminal:eof"] = function(view) send_terminal_key(view, "d", "\x04") end,
  ["terminal:suspend"] = function(view) send_terminal_key(view, "z", "\x1A") end,
  ["terminal:tab"] = function(view) send_terminal_key(view, "tab", "\t") end,
  ["terminal:paste"] = function(view)
    if view.terminal:paste_mode() == "bracketed" then
      view:input("\x1B[200~" .. system.get_clipboard() .. "\x1B[201~")
    else
      view:input(system.get_clipboard())
    end
  end,
  ["terminal:page-up"] = function(view) send_terminal_key(view, "pageup", "\x1B[5~") end,
  ["terminal:page-down"] = function(view) send_terminal_key(view, "pagedown", "\x1B[6~") end,
  ["terminal:scroll-up"] = function(view) view.terminal:scrollback(view.terminal:scrollback() + view.lines) end,
  ["terminal:scroll-down"] = function(view) view.terminal:scrollback(view.terminal:scrollback() - view.lines) end,
  ["terminal:scroll-to-end"] = function(view) view.terminal:scrollback(0) end,
  ["terminal:scroll-to-top"] = function(view) view.terminal:scrollback(view.options.scrollback_limit) end,
  ["terminal:up"] = function(view) send_terminal_key(view, "up", view.terminal:cursor_keys_mode() == "application" and "\x1BOA" or "\x1B[A") end,
  ["terminal:down"] = function(view) send_terminal_key(view, "down", view.terminal:cursor_keys_mode() == "application" and "\x1BOB" or "\x1B[B") end,
  ["terminal:left"] = function(view) send_terminal_key(view, "left", view.terminal:cursor_keys_mode() == "application" and "\x1BOD" or "\x1B[D") end,
  ["terminal:right"] = function(view) send_terminal_key(view, "right", view.terminal:cursor_keys_mode() == "application" and "\x1BOC" or "\x1B[C") end,
  ["terminal:jump-up"] = function(view) send_terminal_key(view, "up", "\x1B[1;5A") end,
  ["terminal:jump-down"] = function(view) send_terminal_key(view, "down", "\x1B[1;5B") end,
  ["terminal:jump-right"] = function(view) send_terminal_key(view, "right", "\x1B[1;5C") end,
  ["terminal:jump-left"] = function(view) send_terminal_key(view, "left", "\x1B[1;5D") end,
  ["terminal:home"] = function(view) send_terminal_key(view, "home", view.terminal:cursor_keys_mode() == "application" and "\x1BOH" or "\x1B[H") end,
  ["terminal:end"] = function(view) send_terminal_key(view, "end", view.terminal:cursor_keys_mode() == "application" and "\x1BOF" or "\x1B[F") end,
  ["terminal:f1"]  = function(view) send_terminal_key(view, "f1", "\x1BOP") end,
  ["terminal:f2"]  = function(view) send_terminal_key(view, "f2", "\x1BOQ") end,
  ["terminal:f3"]  = function(view) send_terminal_key(view, "f3", "\x1BOR") end,
  ["terminal:f4"]  = function(view) send_terminal_key(view, "f4", "\x1BOS") end,
  ["terminal:f5"]  = function(view) send_terminal_key(view, "f5", "\x1B[15~") end,
  ["terminal:f6"]  = function(view) send_terminal_key(view, "f6", "\x1B[17~") end,
  ["terminal:f7"]  = function(view) send_terminal_key(view, "f7", "\x1B[18~") end,
  ["terminal:f8"]  = function(view) send_terminal_key(view, "f8", "\x1B[19~") end,
  ["terminal:f9"]  = function(view) send_terminal_key(view, "f9", "\x1B[20~") end,
  ["terminal:f10"]  = function(view) send_terminal_key(view, "f10", "\x1B[21~") end,
  ["terminal:f11"]  = function(view) send_terminal_key(view, "f11", "\x1B[23~") end,
  ["terminal:f12"]  = function(view) send_terminal_key(view, "f12", "\x1B[24~") end,
  ["terminal:escape"]  = function(view) send_terminal_key(view, "escape", "\x1B") end,
  ["terminal:start"] = function(view) send_terminal_key(view, "s", "\x13") end,
  ["terminal:stop"] = function(view) send_terminal_key(view, "q", "\x11") end,
  ["terminal:cancel"] = function(view) send_terminal_key(view, "x", "\x18") end,
  ["terminal:start-of-heading"] = function(view) send_terminal_key(view, "a", "\x01") end,
  ["terminal:start-of-text"] = function(view) send_terminal_key(view, "b", "\x02") end,
  ["terminal:enquiry"] = function(view) send_terminal_key(view, "e", "\x05") end,
  ["terminal:acknowledge"] = function(view) send_terminal_key(view, "f", "\x06") end,
  ["terminal:bell"] = function(view) send_terminal_key(view, "g", "\x07") end,
  ["terminal:vertical-tab"] = function(view) send_terminal_key(view, "k", "\x0B") end,
  ["terminal:redraw"] = function(view) send_terminal_key(view, "l", "\x0C") end,
  ["terminal:carriage-feed"] = function(view) send_terminal_key(view, "m", "\x0D") end,
  ["terminal:shift-out"] = function(view) send_terminal_key(view, "n", "\x0E") end,
  ["terminal:shift-in"] = function(view) send_terminal_key(view, "o", "\x0F") end,
  ["terminal:data-line-escape"] = function(view) send_terminal_key(view, "p", "\x10") end,
  ["terminal:history"] = function(view) send_terminal_key(view, "r", "\x12") end,
  ["terminal:transpose"] = function(view) send_terminal_key(view, "t", "\x14") end,
  ["terminal:negative-acknowledge"] = function(view) send_terminal_key(view, "u", "\x15") end,
  ["terminal:synchronous-idel"] = function(view) send_terminal_key(view, "v", "\x16") end,
  ["terminal:end-of-transmission-block"] = function(view) send_terminal_key(view, "w", "\x17") end,
  ["terminal:end-of-medium"] = function(view) send_terminal_key(view, "y", "\x19") end,
  ["terminal:file-separator"] = function(view) send_terminal_key(view, "\\", "\x1C") end,
  ["terminal:group-separator"] = function(view) send_terminal_key(view, "]", "\x1D") end,
  ["terminal:clear"] = function(view)
    view.terminal:clear()
    view:invalidate_output_cache()
    view:input(view.options.newline)
  end,
  ["terminal:clear-scrollback"] = function(view)
    view.terminal:clear_scrollback()
    view:invalidate_output_cache()
  end,
  ["terminal:reset"] = function(view)
    view.terminal:reset()
    view:invalidate_output_cache()
  end,
  ["terminal:select-all"] = function(view)
    local _, total_scrollback = view.terminal:scrollback()
    view.selection = { 0, -total_scrollback, view.columns, view.lines - 1 }
    view.terminal:scrollback(0)
    core.redraw = true
  end,
  ["terminal:send-interrupt"] = function(view) view:input("\x03") end,
  ["terminal:send-eof"] = function(view) view:input("\x04") end,
  ["terminal:increase-font-size"] = function(view) adjust_font_size(view, 1) end,
  ["terminal:decrease-font-size"] = function(view) adjust_font_size(view, -1) end,
  ["terminal:restart-local-session"] = function(view)
    if view.session and view.session.capabilities.local_process then view:restart() end
  end,
  ["terminal:new"] = function()
    local view = TerminalView(config.plugins.terminal)
    core.root_view:get_active_node_default():add_view(view)
    core.set_active_view(view)
  end,
  ["terminal:close-tab"] = function(view) view:close() end
});
if system.get_primary_selection then -- check for master vs. 2.1.7
  command.add(active_terminal_predicate, {
    ["terminal:primary-paste"] = function(view)
      if view.terminal:paste_mode() == "bracketed" then
        view:input("\x1B[200~" .. system.get_primary_selection() .. "\x1B[201~")
      else
        view:input(system.get_primary_selection())
      end
    end
  })
end

command.add(function()
  return is_terminal_view(core.active_view) and core.active_view.selection and #core.active_view.selection == 4
end, {
  ["terminal:copy"] = function()
    local selection = core.active_view:sorted_selection()
    system.set_clipboard(core.active_view:get_text(selection[2], selection[1], selection[4], selection[3]))
  end
})

local function toggle_drawer(swap)
  if not core.terminal_view_node or not core.terminal_view then
    core.terminal_view = TerminalView(config.plugins.terminal)
    core.terminal_view_node = core.root_view:get_active_node_default():split("down", core.terminal_view, { y = true }, true)
    core.terminal_view_closed = core.terminal_view.size.y
  end
  if not swap or core.terminal_view_closed then
    if core.terminal_view_closed then
      core.terminal_view_node:resize("y", core.terminal_view_closed)
      core.terminal_view_closed = nil
      core.set_active_view(core.terminal_view)
    else
      core.terminal_view_closed = core.terminal_view.size.y
      core.terminal_view_node:resize("y", 0)
      if core.last_active_view then core.set_active_view(core.last_active_view) end
    end
  else
    core.set_active_view(core.active_view == core.terminal_view and core.last_active_view or core.terminal_view)
  end
end

command.add(nil, {
  ["terminal:toggle-drawer"] = function()
    toggle_drawer(false)
  end,
  ["terminal:swap-drawer"] = function()
    toggle_drawer(true)
  end,
  ["terminal:execute"] = function(text)
    local open_drawer = function(text)
      if not core.terminal_view then command.perform("terminal:toggle-drawer") end
      local target_view = is_terminal_view(core.active_view) and core.active_view or core.terminal_view
      target_view:input(text .. target_view.options.newline)
    end
    if not text then
      core.command_view:enter("Execute Command", { submit = open_drawer })
    else
      open_drawer(text)
    end
  end,
  ["terminal:open-tab"] = function()
    local tv = TerminalView(config.plugins.terminal)
    core.root_view:get_active_node_default():add_view(tv)
  end
})
command.add(function() return core.terminal_view and core.active_view ~= core.terminal_view end, {
  ["terminal:focus"] = function()
    core.set_active_view(core.terminal_view)
  end
})

core.status_view:add_item({
  predicate = function()
    return is_terminal_view(core.active_view) and core.active_view.terminal
  end,
  name = "terminal:info",
  alignment = StatusView.Item.RIGHT,
  get_item = function()
    local dv = core.active_view
    local x, y = dv.terminal:size()
    return {
      style.text, style.font, (dv.terminal:name() or config.plugins.terminal.shell),
      style.dim, style.font, core.status_view.separator2,
      style.text, style.font, x .. "x" .. y
    }
  end
})

-- add in the terminal metacommands not related to actually sending things to the temrinal first
keymap.add({
  ["ctrl+shift+`"] = "terminal:open-tab",
  ["alt+t"]  = "terminal:swap-drawer",
  ["shift+alt+t"]  = "terminal:toggle-drawer"
})
local keys = {
  ["return"] = "terminal:return",
  ["ctrl+return"] = "terminal:return",
  ["shift+return"] = "terminal:return",
  ["keypad enter"] = "terminal:return",
  ["ctrl+keypad enter"] = "terminal:return",
  ["shift+keypad enter"] = "terminal:return",
  ["enter"] = "terminal:enter",
  ["backspace"] = "terminal:backspace",
  ["shift+backspace"] = "terminal:backspace",
  ["delete"] = "terminal:delete",
  ["ctrl+backspace"] = "terminal:ctrl-backspace",
  ["alt+backspace"] = "terminal:alt-backspace",
  ["ctrl+shift+c"] = "terminal:copy",
  ["ctrl+shift+v"] = "terminal:paste",
  ["ctrl+shift+a"] = "terminal:select-all",
  ["mclick"] = "terminal:primary-paste",
  ["wheel"] = "terminal:scroll",
  ["tab"] = "terminal:tab",
  ["pageup"] = "terminal:page-up",
  ["pagedown"] = "terminal:page-down",
  ["shift+pageup"] = "terminal:scroll-up",
  ["shift+pagedown"] = "terminal:scroll-down",
  ["shift+end"] = "terminal:scroll-to-end",
  ["shift+home"] = "terminal:scroll-to-top",
  ["up"] = "terminal:up",
  ["down"] = "terminal:down",
  ["left"] = "terminal:left",
  ["right"] = "terminal:right",
  ["ctrl+up"] = "terminal:jump-up",
  ["ctrl+down"] = "terminal:jump-down",
  ["ctrl+left"] = "terminal:jump-left",
  ["ctrl+right"] = "terminal:jump-right",
  ["alt+left"] = "terminal:jump-left",
  ["alt+right"] = "terminal:jump-right",
  ["home"] = "terminal:home",
  ["end"] = "terminal:end",
  ["f1"] = "terminal:f1",
  ["f2"] = "terminal:f2",
  ["f3"] = "terminal:f3",
  ["f4"] = "terminal:f4",
  ["f5"] = "terminal:f5",
  ["f6"] = "terminal:f6",
  ["f7"] = "terminal:f7",
  ["f8"] = "terminal:f8",
  ["f9"] = "terminal:f9",
  ["f10"] = "terminal:f10",
  ["f11"] = "terminal:f11",
  ["f12"] = "terminal:f11",
  ["escape"] = "terminal:escape",
  ["ctrl+a"] = "terminal:start-of-heading",
  ["ctrl+b"] = "terminal:start-of-text",
  ["ctrl+c"] = "terminal:break",
  ["ctrl+d"] = "terminal:eof",
  ["ctrl+e"] = "terminal:enquiry",
  ["ctrl+f"] = "terminal:acknowledge",
  ["ctrl+g"] = "terminal:bell",
  ["ctrl+h"] = "terminal:backspace",
  ["ctrl+i"] = "terminal:tab",
  ["ctrl+j"] = "terminal:return",
  ["ctrl+k"] = "terminal:vertical-tab",
  ["ctrl+l"] = "terminal:redraw",
  ["ctrl+m"] = "terminal:carriage-feed",
  ["ctrl+n"] = "terminal:shift-out",
  ["ctrl+o"] = "terminal:shift-in",
  ["ctrl+p"] = "terminal:data-line-escape",
  ["ctrl+q"] = "terminal:stop",
  ["ctrl+r"] = "terminal:history",
  ["ctrl+s"] = "terminal:start",
  ["ctrl+t"] = "terminal:transpose",
  ["ctrl+u"] = "terminal:negative-acknowledge",
  ["ctrl+v"] = "terminal:synchronous-idel",
  ["ctrl+w"] = "terminal:end-of-transmission-block",
  ["ctrl+x"] = "terminal:cancel",
  ["ctrl+y"] = "terminal:end-of-medium",
  ["ctrl+z"] = "terminal:suspend",
  ["ctrl+["] = "terminal:escape",
  ["ctrl+\\"] = "terminal:file-separator",
  ["ctrl+]"] = "terminal:group-separator"
}
local non_omitted_keys = {}
for k,v in pairs(keys) do if config.plugins.terminal.omit_escapes == nil or not k:find(config.plugins.terminal.omit_escapes) then non_omitted_keys[k] = v end end
keymap.add(non_omitted_keys)

local terminal_search_keys = {
  [PLATFORM == "Mac OS X" and "cmd+shift+f" or "ctrl+shift+f"] = "terminal:search",
  ["f3"] = "terminal:search-next",
  ["shift+f3"] = "terminal:search-previous",
  ["ctrl+i"] = "terminal:search-toggle-case-sensitive"
}
keymap.add(terminal_search_keys)

if config.plugins.terminal.inversion_key then
  local settings = {}
  local commands = {}
  for i = string.byte('a'), string.byte('z') do
    local keymaps = {}
    for i,v in ipairs(keymap.map["ctrl+" .. string.char(i)] or {}) do
      if not v:find("terminal") then
        table.insert(keymaps, "terminal:" .. v)
        commands["terminal:" .. v] = function(...) command.perform(v, ...) end
      end
    end
    if #keymaps > 0 and not keymap.map["ctrl+" .. config.plugins.terminal.inversion_key .. "+" .. string.char(i)] then
      settings["ctrl+" .. config.plugins.terminal.inversion_key .. "+" .. string.char(i)] = keymaps
    end
  end
  command.add(active_terminal_predicate, commands)
  keymap.add(settings)
end

local function copy_array(values)
  local result = {}
  for i, value in ipairs(values or {}) do result[i] = value end
  return result
end

local function copy_map(values)
  local result = {}
  for key, value in pairs(values or {}) do result[key] = value end
  return result
end

local function normalize_launch_spec(spec)
  spec = spec or {}
  local profile = spec.profile
  if type(profile) == "string" then
    profile = config.plugins.terminal.profiles[profile]
    if not profile then error("unknown terminal profile: " .. spec.profile) end
  end
  profile = profile or {}

  local environment = copy_map(profile.environment)
  for key, value in pairs(spec.environment or {}) do environment[key] = value end
  local command = spec.command or spec.executable or spec.shell
    or profile.command or profile.executable or profile.shell
    or config.plugins.terminal.shell
  local args = spec.args or spec.arguments or profile.args or profile.arguments
    or config.plugins.terminal.arguments

  local result = copy_map(spec)
  result.profile = nil
  result.command = command
  result.executable = command
  result.shell = command
  result.args = copy_array(args)
  result.arguments = copy_array(args)
  result.environment = environment
  result.cwd = spec.cwd or profile.cwd
  result.term = spec.term or profile.term or config.plugins.terminal.term
  result.scrollback_limit = spec.scrollback_limit or profile.scrollback_limit
    or config.plugins.terminal.scrollback_limit
  result.debug = spec.debug
  if result.debug == nil then result.debug = profile.debug or config.plugins.terminal.debug end
  if result.terminate_on_detach == nil then
    result.terminate_on_detach = profile.terminate_on_detach
  end
  return result
end

local function register_profile(name, profile)
  if type(name) ~= "string" or name == "" then error("terminal profile name is required") end
  if type(profile) ~= "table" then error("terminal profile must be a table") end
  config.plugins.terminal.profiles[name] = copy_map(profile)
  return config.plugins.terminal.profiles[name]
end

local function new_emulator(options)
  return Emulator(options)
end

local function open_view(view, options)
  options = options or {}
  local node = options.node or core.root_view:get_active_node_default()
  node:add_view(view)
  if options.activate ~= false then core.set_active_view(view) end
  return view
end

local function open_session(session, options)
  options = options or {}
  if not session
      or type(session.poll_events) ~= "function"
      or type(session.apply_checkpoint) ~= "function" then
    session = Session(session)
  end
  local view_options = copy_map(options)
  view_options.node = nil
  view_options.activate = nil
  view_options.session = session
  view_options.emulator = options.emulator or session.emulator
  return open_view(TerminalView(view_options), options)
end

local function open_local(spec)
  local launch = normalize_launch_spec(spec)
  launch.backend = "local"
  return open_view(TerminalView(launch), spec)
end

local function supported_capabilities()
  return {
    sessions = true,
    emulator = true,
    local_backend = true,
    profiles = true,
    replay = true,
    persistent = false
  }
end

return {
  class = TerminalView,
  Session = Session,
  Emulator = Emulator,
  open_local = open_local,
  open_session = open_session,
  new_emulator = new_emulator,
  register_profile = register_profile,
  normalize_launch_spec = normalize_launch_spec,
  supported_capabilities = supported_capabilities
}
