// A package's Swift, exported to C with `@c` (Swift 6.3, SE-0495) and bound by kama as any C function is (KR-410).
// It calls `wrapping`, declared in helper.swift: a package's Swift files are one module.
@c(greet_add)
public func add(_ a: Int32, _ b: Int32) -> Int32 { return wrapping(a, b) }

// Characters, as Swift counts them: "héllo" is five, where C's strlen counts six bytes.
@c(greet_count)
public func count(_ s: UnsafePointer<CChar>) -> Int32 { return Int32(String(cString: s).count) }
