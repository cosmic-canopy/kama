`heavy` is not `int32`'s own constructor — it comes from the contract `Weighable`, so reach it through a bound: `T.heavy(…)` in a function bounded `T: Weighable<T>`
