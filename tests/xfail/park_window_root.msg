This one (a window over `this.xs`) reaches `std::concurrent::join<int32>` -> `std::concurrent::Fiber<int32>::finish` -> a fiber's join, which switches fibers
