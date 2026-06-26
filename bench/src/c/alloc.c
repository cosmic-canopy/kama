#include <stdint.h>
#include <stdlib.h>
int main(void){
    uint64_t total=0;
    for(int iter=0; iter<2000; iter++){
        int32_t* xs=NULL; size_t len=0, cap=0;
        for(int32_t j=1; j<=1000; j++){
            if(len==cap){ cap = cap?cap*2:4; xs=(int32_t*)realloc(xs, cap*sizeof(int32_t)); }
            xs[len++]=j;
        }
        uint64_t s=0; for(size_t k=0;k<len;k++) s+=(uint64_t)xs[k];
        free(xs);
        total+=s;
    }
    return (int)(total%256);
}
