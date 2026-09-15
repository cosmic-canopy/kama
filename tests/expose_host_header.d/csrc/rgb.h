#ifndef KAMA_TEST_HOSTHDR_RGB_H
#define KAMA_TEST_HOSTHDR_RGB_H
/* A C-owned layout: kama binds it with `type extern value`, and the generated header includes this file
   rather than redefining the struct. */
typedef struct Rgb { unsigned char r, g, b; } Rgb;
int host_run(void);
#endif
