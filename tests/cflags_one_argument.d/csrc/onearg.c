/* Each manifest `cflags` entry is ONE argument, exactly as written (KPG-7): a shell used to strip the quotes from
   the header name, and would have split the greeting at its spaces and run `c` as a second command at the `;`. */
#include "onearg.h"
#include ONEARG_CONFIG_FILE
#include <string.h>
int32_t onearg_answer(void) { return ONEARG_BASE + (int32_t)strlen(ONEARG_GREETING); }   /* 30 + 10 */
