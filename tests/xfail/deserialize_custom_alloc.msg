implements it only `when [T: Deserializable<T>, A: default]`, and `std::collections::BumpAllocator` (for `A`) does not satisfy `default`
