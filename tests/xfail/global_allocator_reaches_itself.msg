reaches the global allocator it implements, through `GlobalHeap::allocate` -> `Pool::allocate` -> `GlobalAllocator::allocate` -> `GlobalHeap::allocate`
