This one (a window over `this.xs`) reaches `std::concurrent::Fiber<int32>::join` -> `std::concurrent::Fiber<int32>::finish` -> a fiber's join, which switches fibers
