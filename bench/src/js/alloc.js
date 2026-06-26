let total = 0;
for (let iter = 0; iter < 2000; iter++) {
    const xs = [];
    for (let j = 1; j <= 1000; j++) xs.push(j);
    let s = 0; for (const v of xs) s += v;
    total += s;
}
process.exit(total % 256);
