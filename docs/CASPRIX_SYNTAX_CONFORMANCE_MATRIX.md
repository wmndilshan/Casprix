# CASPRIX v1 Syntax Conformance Matrix

This matrix defines parser/type-checker acceptance criteria for core syntax governance.

## 1. Classification Buckets

- **A**: Must-accept canonical syntax
- **B**: Accept with normalization warning
- **C**: Reject at parse stage
- **D**: Reject at semantic stage

## 2. Variables and Constants

| Case | Sample | Class | Notes |
|---|---|---|---|
| Immutable local | `let x: int = 1;` | A | Canonical |
| Mutable local | `mut x: int = 1;` | A | Explicit mutable binding |
| Inferred mutable local | `x := 1;` | A | Canonical mutable inference |
| Const compile-time | `const X: int = 4;` | A | Must be constexpr |
| Missing initializer | `let x: int;` | D | Forbidden in v1 |
| Forbidden legacy `var` | `var x = 1;` | C | Not part of the normalized surface |

## 3. Functions and Methods

| Case | Sample | Class | Notes |
|---|---|---|---|
| Block function | `func add(a: int, b: int) -> int { return a+b; }` | A | Canonical |
| Short function | `func add(a: int, b: int) -> int => a+b;` | A | Canonical sugar |
| Async function | `async func f() -> i64 { ... }` | A | Parsed and lowered to a state machine |
| Missing return path | `func f() -> int { }` | D | Non-void must return |
| Comptime async | `comptime async func f() -> int { ... }` | C | `comptime` not implemented |

## 4. Structs and Classes

| Case | Sample | Class | Notes |
|---|---|---|---|
| Struct with typed fields | `struct P { x: f32; y: f32; }` | A | Canonical |
| Struct method receiver | `func len(self: &P) -> f32 => ...;` | A | Canonical |
| Immutable class field | `public let name: string;` | A | Canonical |
| Mutable class field | `private mut count: int;` | A | Canonical |
| Static const field | `public static const count: int = 0;` | A | Canonical |
| Legacy `field` keyword | `public field name: string;` | B | Accepted for compatibility, not canonical |
| Bare class field | `public name: string;` | B | Accepted for compatibility, not canonical |

## 5. Visibility and Modules

| Case | Sample | Class | Notes |
|---|---|---|---|
| Public decl | `public func f() -> void {}` | A | Canonical |
| Private decl | `private func f() -> void {}` | A | Canonical |
| Module import | `import "std/io";` | A | Canonical |
| Alias import | `import "math/linear" as ml;` | C | Not yet accepted by parser |
| Internal visibility | `internal func f() {}` | C | Not in v1 core |

## 6. Arrays and Slices

| Case | Sample | Class | Notes |
|---|---|---|---|
| Fixed array type | `let a: [int; 5] = [0,0,0,0,0];` | A | Canonical |
| Array literal inferred | `let a = [1,2,3];` | A | Canonical |
| Slice type | `let s: [int] = a[0..3];` | A | Canonical view |
| Uninitialized slice | `let s: [int];` | D | No dangling view |
| Mixed rank mismatch | `let x: [int;3] = [1,2];` | D | Length mismatch |

## 7. Control Flow and Match

| Case | Sample | Class | Notes |
|---|---|---|---|
| If/else | `if c { ... } else { ... }` | A | Canonical |
| For-range | `for i in 0..10 { ... }` | A | Canonical |
| Match exhaustive | `match x { 0 => ..., _ => ... }` | A | Canonical |
| Non-exhaustive match on enum | `match e { A => ... }` | D | Semantic reject |

## 8. Closures

| Case | Sample | Class | Notes |
|---|---|---|---|
| Typed closure | `let f = |a: int| -> int { return a+1; };` | A | Canonical |
| Expression closure | `let f = |a: int, b: int| => a + b;` | A | Canonical |
| Ambiguous empty closure params | `let f = || -> int { 1 };` | A | Allowed if parser supports |
| First-class closure call | `let f = |a: int| => a + 1; f(1);` | A | Works for non-capturing and read-only-capture closures; mutable-capture closures work when non-escaping |
| Return a capturing closure | `func g() -> lambda() -> int { return \|:\| => c }` | C | Rejected with a diagnostic; not yet implemented |
| Hidden capture allocation syntax | compiler-inserted boxing | D | Must be explicit by semantics |

## 9. Error Handling

| Case | Sample | Class | Notes |
|---|---|---|---|
| Result return | `func read() -> Result<int, Error> { ... }` | A | Canonical |
| Try propagation | `let x = parse()?;` | C | `?` is tokenized but not parsed |
| Throw keyword | `throw err;` | A | Canonical |

## 10. Async/Await

| Case | Sample | Class | Notes |
|---|---|---|---|
| Await call | `let v = await fetch();` | A | Valid inside an `async func` |
| Await outside async | `func f() { let x = await g(); }` | D | Rejected: `await` outside an async function |
| Spawn statement | `spawn worker();` | C | Parsed; rejected with a "not implemented yet" diagnostic |
| Structured scope | `scope { ... }` | C | Not implemented |

## 11. Ownership/Borrowing

| Case | Sample | Class | Notes |
|---|---|---|---|
| Shared borrow | `func f(x: &T) -> void {}` | A | Canonical |
| Mutable borrow | `func f(x: &mut T) -> void {}` | A | Canonical |
| Illegal aliased mutation | overlapping `&mut` borrows | D | Borrow checker reject |

## 12. Generics and Traits

| Case | Sample | Class | Notes |
|---|---|---|---|
| Generic function | `func id<T>(x: T) -> T => x;` | A | Canonical |
| Trait bound | `func sort<T: Ord>(...) {}` | A | Canonical |
| Overlapping impl ambiguity | conflicting `impl` | D | Coherence reject |

## 13. Operators and Literals

| Case | Sample | Class | Notes |
|---|---|---|---|
| Built-in arithmetic | `a + b * c` | A | Canonical precedence |
| Trait-based overload | `impl Add for Vec2 { ... }` | A | Canonical |
| New operator token | `operator <=>` | C | Not in v1 |
| Width-explicit literal | `42u64`, `1.0f32` | A | Canonical |

## 14. Performance Governance Rules

The following are automatic governance checks for syntax proposals:

1. Must not introduce hidden allocation semantics.
2. Must not introduce hidden dynamic dispatch in basic expression forms.
3. Must not require context-sensitive parser reinterpretation beyond bounded lookahead.
4. Must preserve deterministic lowering to MIR.
5. Must keep declaration axes singular:
- variable mutability: `let/mut/const` and `:=` for inferred mutable locals
- callable declaration: `func`
- visibility: `public/private`

## 15. Scalability Review Checklist (for new syntax)

- Does it preserve grammar LL/LR friendliness?
- Does it preserve source compatibility under additive evolution?
- Does it force global type inference complexity?
- Does it complicate borrow checking across `await`?
- Does it require runtime metadata/reflection in hot paths?
- Does it impact ABI/layout predictability?

If any answer is yes, proposal must include a mitigation plan or be rejected.
