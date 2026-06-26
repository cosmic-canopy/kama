local function clen(n) local s=0 while n~=1 do if n%2==0 then n=n//2 else n=3*n+1 end s=s+1 end return s end
local s=0
for i=1,699999 do s=s+clen(i) end
os.exit(s % 256)
