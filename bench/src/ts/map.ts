const N = 100000, PASSES = 10;
const m = new Map<number, number>();
for (let i = 0; i < N; i++) m.set(i, i * 2);
let sum = 0;
for (let p = 0; p < PASSES; p++)
    for (let i = 0; i < N; i++) {
        const k = (i * 2654435761) % N;
        sum += m.get(k)!;
    }
process.exit(sum % 256);
export {};
