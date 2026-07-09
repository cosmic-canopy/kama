#include <unordered_map>
#include <cstdint>
int main() {
    const long long N = 100000, PASSES = 10;
    std::unordered_map<int, long long> m;
    m.reserve(N * 2);
    for (long long i = 0; i < N; i++) m[(int)i] = i * 2;
    uint64_t sum = 0;
    for (long long p = 0; p < PASSES; p++)
        for (long long i = 0; i < N; i++) {
            int k = (int)((i * 2654435761LL) % N);
            sum += (uint64_t)m[k];
        }
    return (int)(sum % 256);
}
