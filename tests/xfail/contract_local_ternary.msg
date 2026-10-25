`c` borrows its object, and `pick ? x : y` is a value, which nothing holds beyond this statement. Choose the object with `if`/`else` (`Counts c = x; if (!(pick)) { c = y; }`)
