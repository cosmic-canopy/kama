-- dispatch — dynamic dispatch over a heterogeneous table of shapes built at runtime.
local function make_circle(r) return { area = function() return r*r end } end
local function make_square(s) return { area = function() return s*s end } end
local N = 512
local shapes = {}
for j=0,N-1 do if j%2==0 then shapes[j] = make_circle(3) else shapes[j] = make_square(4) end end
local sum = 0
for i=0,7999999 do sum = sum + shapes[i%N].area() end
os.exit(sum % 256)
