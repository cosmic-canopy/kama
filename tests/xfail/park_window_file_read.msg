This one (a window over `this.xs`) reaches `std::fs::File::read` -> a fiber's wait on a pipe or terminal, which switches fibers
