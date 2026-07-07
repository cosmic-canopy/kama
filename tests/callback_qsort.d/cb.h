/* The comparator type C's qsort expects (const-qualified — kama can't spell that
   until M24). A kama FunctionPtr is cast to it at the C edge: cast<CompareFn>(c). */
typedef int (*CompareFn)(const void*, const void*);
