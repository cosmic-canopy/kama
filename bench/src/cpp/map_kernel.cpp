#include <cstdint>
#include <vector>
// Equal-workload kernel — see bench/src/c/map_kernel.c. Same open-addressing map, same hash, same
// fixed prealloc, no stdlib map (C++'s `unordered_map` is measured in the `map` row instead).
struct Map {
    std::vector<int32_t> keys; std::vector<int64_t> vals; std::vector<uint8_t> used; int32_t cap;
    explicit Map(int32_t c) : keys(c), vals(c), used(c, 0), cap(c) {}
    int32_t slotFor(int32_t k) const {
        uint64_t x = (uint64_t)(uint32_t)k * 2654435761u;             // hash
        return (int32_t)(x % (uint64_t)cap);
    }
    void put(int32_t k, int64_t v) {
        int32_t i = slotFor(k);
        while (used[i]) { if (keys[i] == k) { vals[i] = v; return; } i++; if (i >= cap) i = 0; }
        keys[i] = k; vals[i] = v; used[i] = 1;
    }
    int64_t get(int32_t k) const {
        int32_t i = slotFor(k);
        while (used[i]) { if (keys[i] == k) return vals[i]; i++; if (i >= cap) i = 0; }
        return 0;
    }
};
int main() {
    const long long N = 100000, PASSES = 10;
    Map m(262144);                                                    // > N / 0.75, power of 2
    for (long long i = 0; i < N; i++) m.put((int32_t)i, i * 2);
    uint64_t sum = 0;
    for (long long p = 0; p < PASSES; p++)
        for (long long i = 0; i < N; i++) {
            int32_t k = (int32_t)((i * 2654435761LL) % N);
            sum += (uint64_t)m.get(k);
        }
    return (int)(sum % 256);
}
