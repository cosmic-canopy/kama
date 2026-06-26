local total = 0
for iter = 1, 2000 do
    local xs = {}
    for j = 1, 1000 do xs[#xs + 1] = j end
    local s = 0
    for k = 1, #xs do s = s + xs[k] end
    total = total + s
end
os.exit(total % 256)
