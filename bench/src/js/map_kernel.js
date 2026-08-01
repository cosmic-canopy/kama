// Equal-workload kernel — see bench/src/c/map_kernel.c. Same open-addressing map, same hash, same
// fixed prealloc, no stdlib Map (JS's `Map` is measured in the `map` row instead). Typed arrays keep
// the element types honest; every intermediate stays < 2^53, so float64 arithmetic is exact.
const CAP = 262144;               // > N / 0.75, power of 2
const N = 100000, PASSES = 10;

const keys = new Int32Array(CAP);
const vals = new Float64Array(CAP);   // int64 values here peak at 199 998 — exact in float64
const used = new Uint8Array(CAP);

function slotFor(k) {
    return ((k >>> 0) * 2654435761) % CAP;   // hash; < 2^53, exact
}

function put(k, v) {
    let i = slotFor(k);
    while (used[i] !== 0) {
        if (keys[i] === k) { vals[i] = v; return; }
        i++; if (i >= CAP) i = 0;
    }
    keys[i] = k; vals[i] = v; used[i] = 1;
}

function get(k) {
    let i = slotFor(k);
    while (used[i] !== 0) {
        if (keys[i] === k) return vals[i];
        i++; if (i >= CAP) i = 0;
    }
    return 0;
}

for (let i = 0; i < N; i++) put(i, i * 2);
let sum = 0;
for (let p = 0; p < PASSES; p++)
    for (let i = 0; i < N; i++) sum += get((i * 2654435761) % N);
process.exit(sum % 256);
