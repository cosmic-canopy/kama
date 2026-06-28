local function add1(x) return x+1 end
local function mul3(x) return x*3 end
local function apply(op, x) return op(x) end
local a, b = add1, mul3
local sum = 0
for i=0,7999999 do if i%2==0 then sum = sum + apply(a, i) else sum = sum + apply(b, i) end end
os.exit(sum % 256)
