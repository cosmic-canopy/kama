`c` borrows its object, and `W.make(n: 2)` is a value, which nothing holds beyond this statement. Bind it to a local first (`W v = W.make(n: 2); c = v;`)
