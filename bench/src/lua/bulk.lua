-- Lua's byte array is a table; `table.move` is its bulk copy.
local function chunk(k) local m = k % 5; if m == 0 then return 7 elseif m == 1 then return 64 elseif m == 2 then return 1000 elseif m == 3 then return 4096 else return 65536 end end
local SRC, TARGET = 65536, 4194304
local src = {}
for i = 0, SRC - 1 do src[i + 1] = (i * 7 + (i >> 8)) & 0xff end
local dst = {}
for i = 1, TARGET do dst[i] = 0 end
local total = 0
for r = 0, 63 do
  local buf, len = {}, 0
  local k, off = r, r * 13
  while len < TARGET do
    local c = chunk(k); k = k + 1
    if c > TARGET - len then c = TARGET - len end
    off = (off + 4099) % (SRC - c + 1)
    table.move(src, off + 1, off + c, len + 1, buf); len = len + c
  end
  table.move(buf, 1, len, 1, dst)
  local s = 0; for i = r, TARGET - 1, 4093 do s = s + dst[i + 1] end
  total = total + s
end
os.exit(total % 256)
