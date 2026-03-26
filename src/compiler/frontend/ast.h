#ifndef AST_H
#define AST_H

#include "casprix/common.h"
#include "compiler/frontend/lexer.h"

// ============================================================================
// Type System - Complete Data Type Taxonomy
// ============================================================================
//
// Type categories:
//   Primitive integers:  i8, i16, i32, i64, i128, u8, u16, u32, u64, u128
//   Floating point:      f16, f32, f64, bf16
//   Other primitives:    bool, char
//   String types:        string (immutable), strbuf (mutable)
//   Void:                void
//   Compound types:      struct, enum, union, class
//   Collection types:    array<T>, [T; N], slice<T>
//   Pointer/ref types:   ptr<T>, rawptr, ref<T>
//   SIMD/math types:     vecN<T>, matN<T>, tensor<T>
//   Function:            func
//   Generic:             T (type parameter)
//
// Memory layout:
//   All primitives are unboxed and stack-allocated by default.
//   Strings use length + pointer layout with UTF-8 encoding.
//   SIMD types map directly to SSE/AVX registers.
//   Tensors use arena or pooled allocation with shape metadata.
//
typedef enum {
    // === Signed Integer Types ===
    TYPE_I8,             // 8-bit signed integer
    TYPE_I16,            // 16-bit signed integer
    TYPE_I32,            // 32-bit signed integer (default 'int')
    TYPE_I64,            // 64-bit signed integer
    TYPE_I128,           // 128-bit signed integer

    // === Unsigned Integer Types ===
    TYPE_U8,             // 8-bit unsigned integer
    TYPE_U16,            // 16-bit unsigned integer
    TYPE_U32,            // 32-bit unsigned integer
    TYPE_U64,            // 64-bit unsigned integer
    TYPE_U128,           // 128-bit unsigned integer

    // === Floating Point Types ===
    TYPE_F16,            // 16-bit half precision
    TYPE_F32,            // 32-bit single precision
    TYPE_F64,            // 64-bit double precision (default 'float')
    TYPE_BF16,           // 16-bit brain float (AI acceleration)

    // === Other Primitive Types ===
    TYPE_BOOL,           // Boolean (true/false)
    TYPE_CHAR,           // Character (UTF-32 code point)

    // === String Types ===
    TYPE_STRING,         // Immutable string (length + ptr, UTF-8)
    TYPE_STRBUF,         // Mutable string buffer

    // === Void ===
    TYPE_VOID,           // No value

    // === Compound Types ===
    TYPE_STRUCT,         // Value type struct
    TYPE_ENUM,           // Tagged union / algebraic data type
    TYPE_UNION,          // Untagged union (unsafe)
    TYPE_CLASS,          // Class instance (heap-allocated, ARC)

    // === Collection Types ===
    TYPE_ARRAY,          // Dynamic array: array<T>
    TYPE_STATIC_ARRAY,   // Fixed-size array: [T; N]
    TYPE_SLICE,          // Borrowed view: slice<T>

    // === Pointer / Reference Types ===
    TYPE_PTR,            // Owned pointer: ptr<T>
    TYPE_RAWPTR,         // Raw untyped pointer: rawptr
    TYPE_REF,            // ARC reference: ref<T>

    // === SIMD / Math Types ===
    TYPE_VEC,            // SIMD vector: vec2<f32>, vec4<f32>, etc.
    TYPE_MAT,            // Matrix: mat2<f32>, mat3<f32>, mat4<f32>
    TYPE_TENSOR,         // N-dimensional tensor: tensor<f32>[3,3]

    // === Function / Closure ===
    TYPE_FUNC,           // Function or closure type

    // === Dynamic / Erased ===
    TYPE_DYN,            // Explicit dynamic/erased handle

    // === Generic Type Parameter ===
    TYPE_GENERIC,        // Type variable (e.g., T)

    // === Special ===
    TYPE_ERROR           // Unresolved / error sentinel
} DataType;

// Backward compatibility aliases: 'int' -> i32, 'float' -> f64
#define TYPE_INT   TYPE_I32
#define TYPE_FLOAT TYPE_F64

// Extended type information for parameterized and compound types
// Used when DataType alone is insufficient (arrays, vectors, tensors, etc.)
typedef struct TypeInfo {
    DataType base;                   // Primary type category

    // For parameterized types: array<T>, ptr<T>, vec<T>, ref<T>, slice<T>
    struct TypeInfo* element_type;   // Element/pointee type (NULL for non-parameterized)

    // For static arrays: [T; N]  and  vecN / matN
    int static_size;                 // Array size N, or vector/matrix dimension (2,3,4,8,16)

    // For tensor types: tensor<f32>[3,3]
    int* tensor_dims;                // Shape dimensions (e.g., [3, 3] or [1, 28, 28])
    int tensor_ndims;                // Number of dimensions

    // For struct/class/enum/union named types
    char* type_name;                 // Name of the type (e.g., "User", "Result")

    // For function types: func(i32, i32) -> i32
    struct TypeInfo** param_types;   // Parameter types
    int param_count;                 // Number of parameters
    struct TypeInfo* return_type;    // Return type
} TypeInfo;

// TypeInfo creation and management
TypeInfo* create_type_info(DataType base);
TypeInfo* create_parameterized_type(DataType base, TypeInfo* element);
TypeInfo* create_static_array_type(TypeInfo* element, int size);
TypeInfo* create_vec_type(TypeInfo* element, int dim);
TypeInfo* create_mat_type(TypeInfo* element, int dim);
TypeInfo* create_tensor_type(TypeInfo* element, int* dims, int ndims);
TypeInfo* create_named_type(DataType base, const char* name);
void free_type_info(TypeInfo* info);
TypeInfo* clone_type_info(const TypeInfo* info);

// Type utility functions
int type_size_bytes(DataType type);          // Size in bytes for primitive types
int type_alignment(DataType type);           // Alignment requirement
bool type_is_integer(DataType type);         // i8..i128, u8..u128
bool type_is_unsigned(DataType type);        // u8..u128
bool type_is_signed(DataType type);          // i8..i128
bool type_is_float(DataType type);           // f16, f32, f64, bf16
bool type_is_numeric(DataType type);         // integer or float
bool type_is_primitive(DataType type);       // numeric, bool, char
bool type_is_simd(DataType type);            // vec, mat
bool type_is_pointer(DataType type);         // ptr, rawptr, ref
bool type_is_collection(DataType type);      // array, static_array, slice
const char* datatype_to_string(DataType type);

// Extended type information for classes
typedef struct ClassType {
    char* class_name;
    struct ClassType* parent;  // NULL for base classes
} ClassType;

// ============================================================================
// Ownership Model
// ============================================================================
//
// Casprix uses a hybrid ownership model:
//   OWNED      — default; the variable is the sole owner, freed at scope end
//   BORROW     — &T: read-only reference, does not own the value
//   BORROW_MUT — &mut T: exclusive mutable reference
//   WEAK       — Weak<T>: non-owning reference, may become null if owner drops
//   MOVED      — source variable after a `move` expression (cannot be used)
//
typedef enum {
    OWNERSHIP_OWNED,       // Default: variable owns the value
    OWNERSHIP_BORROW,      // &T — shared immutable borrow
    OWNERSHIP_BORROW_MUT,  // &mut T — exclusive mutable borrow
    OWNERSHIP_WEAK,        // Weak<T> — weak (non-owning) reference
    OWNERSHIP_MOVED        // Value moved out; further use is a compile error
} OwnershipMode;

// Expression nodes
typedef enum {
    EXPR_BINARY,
    EXPR_UNARY,
    EXPR_LITERAL,
    EXPR_VARIABLE,
    EXPR_CALL,
    EXPR_MEMBER_ACCESS,  // obj.field or obj.method()
    EXPR_STATIC_ACCESS,  // ClassName.staticMember or ClassName.staticMethod()
    EXPR_THIS,           // this keyword
    EXPR_SUPER,          // Super.field or Super.method() - parent class access
    EXPR_NEW,            // Object instantiation: New ClassName(args)
    EXPR_INDEX,          // Array indexing: arr[index]
    EXPR_LAMBDA,         // Lambda: |x: Int, y: Int| => x + y
    EXPR_GENERIC_INST    // Generic instantiation: List<Int>
} ExprType;

typedef struct Expr Expr;

typedef struct {
    Expr* left;
    TokenType operator;
    Expr* right;
} BinaryExpr;

typedef struct {
    TokenType operator;
    Expr* operand;
} UnaryExpr;

typedef struct {
    DataType type;
    union {
        int64_t int_value;
        double float_value;
        char* string_value;
        bool bool_value;
    } value;
} LiteralExpr;

typedef struct {
    char* name;
    bool  is_move;   // True when prefixed with `move`: `y = move x`
} VariableExpr;

typedef struct {
    Expr* callee;            // Callable expression being invoked
    char* name;              // Cached direct callee name for legacy/direct-call paths
    Expr** arguments;
    int arg_count;
} CallExpr;

typedef struct {
    Expr* object;              // Left side (e.g., dog)
    char* member_name;         // Right side (e.g., speak or age)
    bool is_method_call;       // True if followed by ()
    Expr** arguments;          // For method calls
    int arg_count;
} MemberAccessExpr;

typedef struct {
    char* class_name;          // Class name (e.g., Math)
    char* member_name;         // Static member name (e.g., abs or PI)
    bool is_method_call;       // True if followed by ()
    Expr** arguments;          // For method calls
    int arg_count;
} StaticAccessExpr;

typedef struct {
    char* class_name;          // Type of 'this'
} ThisExpr;

typedef struct {
    char* member_name;         // Parent class member name
    bool is_method_call;       // True if followed by ()
    Expr** arguments;          // For method calls
    int arg_count;
} SuperExpr;

typedef struct {
    char* class_name;          // Class to instantiate
    Expr** arguments;          // Constructor arguments
    int arg_count;
} NewExpr;

typedef struct {
    Expr* array;               // Array expression
    Expr* index;               // Index expression
} IndexExpr;

// Forward declaration for Parameter (used by LambdaExpr)
typedef struct {
    char* name;
    DataType type;
    char* class_name;      // For TYPE_CLASS parameters, the specific class name
    TypeInfo* type_info;   // Extended type info for parameterized types
    OwnershipMode ownership; // OWNED (default), BORROW, BORROW_MUT, WEAK
} Parameter;

// Closure capture mode (user-specified or inferred)
typedef enum {
    CAPTURE_INFER,       // Default: compiler decides
    CAPTURE_BORROW,      // |&| — borrow all captures
    CAPTURE_MOVE,        // |move| — move all captures into closure
    CAPTURE_COPY         // |copy| — copy all captures
} ClosureCaptureMode;

// Lambda/Closure expression: |x: Int, y: Int| => x + y
typedef struct {
    Parameter* parameters;     // Lambda parameters
    int param_count;
    DataType return_type;      // Return type (may be inferred)
    char* return_class_name;   // For class return types
    bool is_expression;        // true = => expr, false = -> Type { block }
    Expr* expr_body;           // For expression lambdas
    struct Stmt* block_body;   // For block lambdas
    ClosureCaptureMode capture_mode; // User-specified capture mode
    // Filled by semantic analysis:
    char** captured_vars;      // Names of captured variables
    DataType* captured_types;  // Types of captured variables
    int capture_count;
    bool has_mutable_capture;  // True if any captured variable is mutated
    int closure_id;            // Unique ID for code generation
} LambdaExpr;

// Generic type instantiation: List<Int>, Map<String, Int>
typedef struct {
    char* base_name;           // Generic class/func name (e.g., "List")
    DataType* type_args;       // Type arguments
    char** type_arg_classes;   // Class names for TYPE_CLASS args
    int type_arg_count;        // Number of type arguments
} GenericInstExpr;

// Type parameter for generic definitions
typedef struct {
    char* name;                // Parameter name (e.g., "T")
    char* constraint;          // Optional constraint class name
    DataType default_type;     // Default type if not specified
    char* default_class;       // Default class name
} TypeParam;

struct Expr {
    ExprType type;
    DataType data_type;  // Type of the expression result
    char* class_name;    // For TYPE_CLASS/TYPE_STRUCT/TYPE_ENUM, the specific type name
    TypeInfo* type_info; // Extended type info for parameterized types (NULL for simple types)
    int line;
    int column;

    union {
        BinaryExpr binary;
        UnaryExpr unary;
        LiteralExpr literal;
        VariableExpr variable;
        CallExpr call;
        MemberAccessExpr member;
        StaticAccessExpr static_access;
        ThisExpr this_expr;
        SuperExpr super_expr;
        NewExpr new_expr;
        IndexExpr index;
        LambdaExpr lambda;
        GenericInstExpr generic_inst;
    } as;
};

// Statement nodes
typedef enum {
    STMT_DECLARATION,
    STMT_CONST_DECL,      // const declaration (compile-time constant)
    STMT_ASSIGNMENT,
    STMT_PRINT,
    STMT_IF,
    STMT_FOR,
    STMT_FOR_IN,          // for x in iterable { }
    STMT_WHILE,
    STMT_FUNCTION,
    STMT_RETURN,
    STMT_BREAK,
    STMT_CONTINUE,
    STMT_EXPR,
    STMT_BLOCK,
    STMT_INCLUDE,
    STMT_CLASS,           // Class declaration
    STMT_EXTERN,          // External function declaration
    STMT_STRUCT,          // Struct declaration
    STMT_ENUM,            // Enum declaration
    STMT_UNION,           // Union declaration
    STMT_TRY,             // try/catch/finally
    STMT_THROW,           // throw expression
    STMT_MATCH,           // match expression
    STMT_TRAIT,           // trait declaration
    STMT_IMPL             // impl block for struct/class methods
} StmtType;

typedef struct Stmt Stmt;

typedef struct {
    char* name;
    DataType type;
    char* class_name;    // For TYPE_CLASS, the specific class name
    Expr* initializer;
    TypeInfo* type_info; // Extended type info for parameterized types (NULL for simple types)
    OwnershipMode ownership; // OWNED (default), BORROW, BORROW_MUT, WEAK
    bool is_const;       // True for const declarations
    bool is_mutable;     // True for mutable declarations (:=, mut, legacy typed locals)
} DeclarationStmt;

typedef struct {
    Expr* target;  // Can be EXPR_VARIABLE or EXPR_MEMBER_ACCESS
    Expr* value;
} AssignmentStmt;

typedef struct {
    Expr* expression;
} PrintStmt;

typedef struct {
    Expr* condition;
    Stmt* then_branch;
    Stmt* else_branch;
} IfStmt;

typedef struct {
    char* variable;
    DataType var_type;
    Expr* initializer;
    Expr* condition;
    Stmt* increment;
    Stmt* body;
} ForStmt;

typedef struct {
    Expr* condition;
    Stmt* body;
} WhileStmt;

typedef struct {
    char* name;
    Parameter* parameters;
    int param_count;
    DataType return_type;
    Stmt* body;
    // Generic support
    TypeParam* type_params;    // Generic type parameters (NULL if not generic)
    int type_param_count;
} FunctionStmt;

typedef struct {
    Expr* value;
} ReturnStmt;

typedef struct {
    // Break has no data - just exits the loop
} BreakStmt;

typedef struct {
    // Continue has no data - just jumps to next iteration
} ContinueStmt;

typedef struct {
    Expr* expression;
} ExprStmt;

typedef struct {
    Stmt** statements;
    int stmt_count;
    bool is_alloc_scope;   // True for alloc { ... } regions
} BlockStmt;

typedef struct {
    char* module_name;
    bool is_import;  // true for Import, false for Include
} IncludeStmt;

typedef struct {
    char* name;
    Parameter* parameters;
    int param_count;
    DataType return_type;
    char* class_name; // For TYPE_CLASS return type
} ExternStmt;

// Class-related structures
typedef enum {
    ACCESS_PUBLIC,
    ACCESS_PRIVATE,
    ACCESS_PROTECTED
} AccessModifier;

typedef struct {
    char* name;
    DataType type;
    char* class_name;          // For TYPE_CLASS fields, the specific class name
    Expr* default_value;       // Optional default (can be NULL)
    bool is_const;             // True for const fields
    bool is_mutable;           // True for mut fields and legacy mutable fields
    bool is_static;            // True for static fields
    AccessModifier access;     // Access modifier (default: PUBLIC)
    OwnershipMode ownership;   // OWNED (default), BORROW, BORROW_MUT, WEAK
} FieldDecl;

typedef struct {
    char* name;
    Parameter* parameters;
    int param_count;
    DataType return_type;
    Stmt* body;
    bool is_constructor;       // True for constructor method (class name or legacy "new")
    bool is_static;            // True for static methods
    bool is_abstract;          // True for abstract methods (no body)
    AccessModifier access;     // Access modifier (default: PUBLIC)
} MethodDecl;

typedef struct {
    char* name;
    char* parent_name;         // NULL if no inheritance
    bool is_abstract;          // True for abstract classes
    FieldDecl* fields;         // Member variables
    int field_count;
    MethodDecl* methods;       // Member functions
    int method_count;
    // Generic support
    TypeParam* type_params;    // Generic type parameters (NULL if not generic)
    int type_param_count;
    // Trait implementation
    char** implements;         // List of trait names this class implements
    int implements_count;
} ClassStmt;

// Struct field (value type, no methods by default)
typedef struct {
    char* name;
    DataType type;
    char* class_name;          // For TYPE_CLASS fields
    TypeInfo* type_info;       // Extended type info
} StructField;

// Struct declaration: struct Name { field: type, ... }
typedef struct {
    char* name;
    StructField* fields;
    int field_count;
    TypeParam* type_params;    // Generic type parameters
    int type_param_count;
} StructStmt;

// Enum variant
typedef struct {
    char* name;
    DataType* payload_types;   // Payload types for variant (NULL if none)
    char** payload_names;      // Optional payload field names
    int payload_count;
    int64_t tag_value;         // Explicit tag value (-1 for auto)
} EnumVariant;

// Enum declaration: enum Name { Variant1, Variant2(T), ... }
typedef struct {
    char* name;
    EnumVariant* variants;
    int variant_count;
    TypeParam* type_params;    // Generic type parameters
    int type_param_count;
} EnumStmt;

// Union field
typedef struct {
    char* name;
    DataType type;
    TypeInfo* type_info;       // Extended type info
} UnionField;

// Union declaration: union Name { field: type, ... }
typedef struct {
    char* name;
    UnionField* fields;
    int field_count;
} UnionStmt;

// For-in loop: for x in iterable { body }
typedef struct {
    char* var_name;        // loop variable name
    DataType var_type;     // variable type (often inferred)
    Expr* iterable;        // the collection expression
    Stmt* body;
} ForInStmt;

// Match arm: pattern => body
typedef struct {
    Expr* pattern;         // NULL = wildcard (_)
    char* variant_name;    // For enum variant matching (e.g. "Red")
    Expr** bind_vars;      // Bound variables in the pattern
    int bind_count;
    Stmt* body;
} MatchArm;

// Match statement: match expr { arm, arm, ... }
typedef struct {
    Expr* subject;         // The expression being matched
    MatchArm* arms;
    int arm_count;
} MatchStmt;

// Catch clause: catch (ex: ExType) { body }
typedef struct {
    char* exception_var;   // variable name (e.g., "ex")
    char* exception_type;  // type name (e.g., "IOException"); NULL = catch-all
    Stmt* body;
} CatchClause;

// Try/catch/finally statement
typedef struct {
    Stmt* try_body;
    CatchClause* catches;
    int catch_count;
    Stmt* finally_body;    // NULL if no finally
} TryStmt;

// Throw statement
typedef struct {
    Expr* value;           // The thrown object/expression
} ThrowStmt;

// Trait method signature (abstract, no body)
typedef struct {
    char* name;
    Parameter* parameters;
    int param_count;
    DataType return_type;
    char* return_class_name;  // For class return types
    bool has_default;         // True if default implementation provided
    Stmt* default_body;       // May be NULL
} TraitMethodDecl;

// Trait declaration: trait Foo { func bar() -> T; }
typedef struct {
    char* name;
    TraitMethodDecl* methods;
    int method_count;
    TypeParam* type_params;
    int type_param_count;
    char** super_traits;      // Trait inheritance
    int super_count;
} TraitStmt;

// Impl block: impl Type { func method(self: &T) -> R { } }
// Also used for trait implementations: impl Type : TraitName { ... }
typedef struct {
    char* target_name;         // Type being extended (e.g., "Vec2")
    char* trait_name;          // NULL for inherent impl, trait name for trait impl
    Stmt** methods;            // Full function declaration statements
    int method_count;
    TypeParam* type_params;    // Generic type parameters
    int type_param_count;
} ImplStmt;

struct Stmt {
    StmtType type;
    int line;
    int column;

    union {
        DeclarationStmt declaration;
        AssignmentStmt assignment;
        PrintStmt print;
        IfStmt if_stmt;
        ForStmt for_stmt;
        ForInStmt for_in_stmt;
        WhileStmt while_stmt;
        FunctionStmt function;
        ReturnStmt return_stmt;
        BreakStmt break_stmt;
        ContinueStmt continue_stmt;
        ExprStmt expr_stmt;
        BlockStmt block;
        IncludeStmt include;
        ClassStmt class_stmt;
        ExternStmt extern_stmt;
        StructStmt struct_stmt;
        EnumStmt enum_stmt;
        UnionStmt union_stmt;
        TryStmt try_stmt;
        ThrowStmt throw_stmt;
        MatchStmt match_stmt;
        TraitStmt trait_stmt;
        ImplStmt impl_stmt;
    } as;
};

// AST creation functions
Expr* create_binary_expr(Expr* left, TokenType op, Expr* right, int line, int col);
Expr* create_unary_expr(TokenType op, Expr* operand, int line, int col);
Expr* create_literal_expr(LiteralExpr literal, int line, int col);
Expr* create_variable_expr(char* name, int line, int col);
Expr* create_call_expr(Expr* callee, Expr** args, int arg_count, int line, int col);
Expr* create_member_access_expr(Expr* object, char* member_name, bool is_method_call,
                                Expr** args, int arg_count, int line, int col);
Expr* create_static_access_expr(char* class_name, char* member_name, bool is_method_call,
                                Expr** args, int arg_count, int line, int col);
Expr* create_this_expr(char* class_name, int line, int col);
Expr* create_new_expr(char* class_name, Expr** arguments, int arg_count, int line, int col);
Expr* create_index_expr(Expr* array, Expr* index, int line, int col);

Stmt* create_declaration_stmt(char* name, DataType type, Expr* init, int line, int col);
Stmt* create_assignment_stmt(Expr* target, Expr* value, int line, int col);
Stmt* create_print_stmt(Expr* expr, int line, int col);
Stmt* create_if_stmt(Expr* cond, Stmt* then_br, Stmt* else_br, int line, int col);
Stmt* create_for_stmt(char* var, DataType type, Expr* init, Expr* cond, 
                      Stmt* incr, Stmt* body, int line, int col);
Stmt* create_while_stmt(Expr* cond, Stmt* body, int line, int col);
Stmt* create_function_stmt(char* name, Parameter* params, int param_count,
                           DataType ret_type, Stmt* body, int line, int col);
Stmt* create_return_stmt(Expr* value, int line, int col);
Stmt* create_break_stmt(int line, int col);
Stmt* create_continue_stmt(int line, int col);
Stmt* create_expr_stmt(Expr* expr, int line, int col);
Stmt* create_block_stmt(Stmt** stmts, int count, int line, int col);
Stmt* create_include_stmt(char* module_name, bool is_import, int line, int col);
Stmt* create_class_stmt(char* name, char* parent_name, FieldDecl* fields, int field_count,
                        MethodDecl* methods, int method_count, int line, int col);
Stmt* create_extern_stmt(char* name, Parameter* params, int param_count,
                         DataType ret_type, char* class_name, int line, int col);
Stmt* create_struct_stmt(char* name, StructField* fields, int field_count, int line, int col);
Stmt* create_enum_stmt(char* name, EnumVariant* variants, int variant_count, int line, int col);
Stmt* create_union_stmt(char* name, UnionField* fields, int field_count, int line, int col);

// Memory management
void free_expr(Expr* expr);
void free_stmt(Stmt* stmt);

// Pretty printing
void print_ast(Stmt** statements, int count);

#endif // AST_H
