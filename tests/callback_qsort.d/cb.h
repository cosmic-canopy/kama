/* The comparator type C's qsort expects. cstar references it as the callback
   type; a cstar function is cast to it via cast<CompareFn>(funcptr(of: ...)). */
typedef int (*CompareFn)(const void*, const void*);
