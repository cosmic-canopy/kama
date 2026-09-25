#include <sys/stat.h>
#include "umask_probe.h"

int32_t kama_test_set_umask(int32_t mask) { return (int32_t)umask((mode_t)mask); }

int32_t kama_test_mode_of(const char* path)
{
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (int32_t)(st.st_mode & 0777);
}
