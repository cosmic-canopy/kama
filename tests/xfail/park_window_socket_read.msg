This one (a window over `this.xs`) reaches `std::net::TcpStream::read` -> `kama_net_drained` -> a fiber's wait on a socket, which switches fibers
