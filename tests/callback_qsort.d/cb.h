/* The comparator type C's qsort expects (const-qualified — cstar can't spell that
   until M24). A cstar FunctionPtr is cast to it at the C edge: cast<CompareFn>(c). */
typedef int (*CompareFn)(const void*, const void*);
