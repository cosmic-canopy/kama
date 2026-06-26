#include <cstdint>
#include <vector>
int main(){
    uint64_t total=0;
    for(int iter=0; iter<2000; iter++){
        std::vector<int32_t> xs;
        for(int32_t j=1;j<=1000;j++) xs.push_back(j);
        uint64_t s=0; for(int32_t v: xs) s+=(uint64_t)v;
        total+=s;
    }
    return (int)(total%256);
}
