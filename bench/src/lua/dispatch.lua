local function make_circle(r) return { area = function() return r*r end } end
local function make_square(s) return { area = function() return s*s end } end
local function measure(sh) return sh.area() end
local c = make_circle(3)
local q = make_square(4)
local sum = 0
for i=0,7999999 do if i%2==0 then sum = sum + measure(c) else sum = sum + measure(q) end end
os.exit(sum % 256)
