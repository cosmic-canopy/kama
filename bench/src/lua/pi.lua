local pi=0.0
local sign=1.0
for i=0,19999999 do local d=2*i+1; pi = pi + sign/d; sign = -sign end
local scaled = pi*4000000000.0
os.exit(math.floor(scaled) % 256)
