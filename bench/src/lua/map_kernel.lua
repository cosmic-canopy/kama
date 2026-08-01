-- Equal-workload kernel -- see bench/src/c/map_kernel.c. Same open-addressing map, same hash, same
-- fixed prealloc, no stdlib table-as-map (Lua's native table is measured in the `map` row instead).
local CAP = 262144        -- > N / 0.75, power of 2
local N, PASSES = 100000, 10

local keys, vals, used = {}, {}, {}
for i = 0, CAP - 1 do used[i] = 0 end

local function slotFor(k)
    return ((k & 0xFFFFFFFF) * 2654435761) % CAP    -- hash (Lua 5.4 has 64-bit integers)
end

local function put(k, v)
    local i = slotFor(k)
    while used[i] ~= 0 do
        if keys[i] == k then vals[i] = v; return end
        i = i + 1; if i >= CAP then i = 0 end
    end
    keys[i] = k; vals[i] = v; used[i] = 1
end

local function get(k)
    local i = slotFor(k)
    while used[i] ~= 0 do
        if keys[i] == k then return vals[i] end
        i = i + 1; if i >= CAP then i = 0 end
    end
    return 0
end

for i = 0, N - 1 do put(i, i * 2) end
local sum = 0
for _ = 1, PASSES do
    for i = 0, N - 1 do
        sum = sum + get((i * 2654435761) % N)
    end
end
os.exit(sum % 256)
