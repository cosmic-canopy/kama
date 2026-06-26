local function fib(n) if n<2 then return n end return fib(n-1)+fib(n-2) end
local s=0
for i=0,31 do s=s+fib(i) end
os.exit(s % 256)
