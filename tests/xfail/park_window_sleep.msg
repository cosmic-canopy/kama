This one (a window over `this.ticks`) reaches `std::time::sleep` -> `kama_sleep_ns` -> a fiber's sleep, which switches fibers
