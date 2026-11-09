#ifndef SEAM_H
#define SEAM_H
#include <stdint.h>
int32_t seam_start(int32_t n, int32_t masked, int32_t overflow);
void*   seam_cell(void);
void*   seam_bell(void);
void    seam_arrive(void);
void    seam_ack(void);
int32_t seam_probe(int32_t mode, int32_t n, int32_t masked);
#endif
