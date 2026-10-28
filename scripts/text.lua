-- gpu.text showcase: ASCII table, scales 1-4, colors, bg box, newlines, a line longer than one draw chunk.

local ascii = {}
for row = 0, 7 do
  local chars = {}
  for c = row * 16, row * 16 + 15 do chars[#chars + 1] = (c >= 32 and c < 127) and string.char(c) or "." end
  ascii[#ascii + 1] = string.format("%02X  %s", row * 16, table.concat(chars, " "))
end
ascii = table.concat(ascii, "\n")
local long = string.rep("0123456789", 20) -- 200 chars: 3 draw chunks

local total = pxb.args.frames_warmup + pxb.args.frames -- --frames 0 = run until the window is closed
for i = 1, pxb.args.frames == 0 and math.maxinteger or total do
  local w, h = gpu.swapchain_size()
  gpu.begin_pass{clear = {0.1, 0.12, 0.18, 1}}
  gpu.text(16, 16, ascii, {scale = 2})
  local y = 16 + 9 * 16
  for s = 1, 4 do
    gpu.text(16, y, string.format("scale %d: The quick brown fox jumps over the lazy dog", s), {scale = s})
    y = y + 8 * s + 4
  end
  gpu.text(16, y + 8, "red", {scale = 3, color = {1, 0.2, 0.2, 1}})
  gpu.text(16 + 4 * 24, y + 8, "green", {scale = 3, color = {0.2, 1, 0.2, 1}})
  gpu.text(16 + 10 * 24, y + 8, "half alpha", {scale = 3, color = {1, 1, 1, 0.5}})
  y = y + 40
  gpu.text(16, y, " bg box \n two lines ", {scale = 3, color = {1, 1, 0, 1}, bg = {0, 0, 0, 0.6}})
  y = y + 2 * 24 + 8
  gpu.text(16, y, "non-ASCII: \xC3\xA9 -> ?? | " .. long, {scale = 2})
  gpu.text(w - 8 * 2 * 12 - 16, h - 32, string.format("frame %6d", i), {scale = 2, color = {0.6, 0.8, 1, 1}})
  gpu.end_pass()
  gpu.present()
end
