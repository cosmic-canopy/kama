local N = 100000
local PASSES = 10
local m = {}
for i = 0, N - 1 do m[i] = i * 2 end
local sum = 0
for _ = 1, PASSES do
    for i = 0, N - 1 do
        local k = (i * 2654435761) % N
        sum = sum + (m[k] or 0)
    end
end
os.exit(sum % 256)
