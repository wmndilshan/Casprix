# CASPRIX v1 Syntax Grammar (EBNF Draft)

This grammar defines the normalized CASPRIX surface syntax for parser implementation and syntax governance.

## 1. Lexical Notes

- Keywords are reserved: `import`, `as`, `public`, `private`, `let`, `mut`, `const`, `func`, `struct`, `class`, `trait`, `impl`, `for`, `in`, `if`, `else`, `while`, `loop`, `match`, `return`, `throw`, `try`, `catch`, `finally`.
- Statement terminator is `;` except for block-terminated declarations and control-flow blocks.
- Line comments: `// ...`
- Block comments: `/* ... */`

Planned or currently unsupported surface forms:
- `async`, `await`, `spawn`
- `where`
- `scope`

## 2. Top-Level

```ebnf
Program          = { TopDecl } EOF ;

TopDecl          = ImportDecl
                 | ConstDecl
                 | FuncDecl
                 | StructDecl
                 | ClassDecl
                 | TraitDecl
                 | ImplDecl ;

ImportDecl       = "import" StringLit ";" ;

QualifiedIdent   = Ident { "." Ident } ;
Ident            = Letter { Letter | Digit | "_" } ;
```

## 3. Visibility and Modifiers

```ebnf
Visibility       = "public" | "private" | "protected" ;
OptVisibility    = [ Visibility ] ;

MemberModifier   = [ Visibility ] [ "static" ] ;
MethodModifier   = MemberModifier [ "abstract" ] ;
FieldBinding     = "let" | "mut" | "const" ;
```

Constraints:
- `static` is valid only inside `class`.
- Canonical class fields use `let`, `mut`, or `const`; legacy `field name: T` and bare `name: T` forms remain compatibility syntax only.

## 4. Declarations

```ebnf
ConstDecl        = "const" Ident [ ":" Type ] "=" Expr ";" ;

LetDecl          = "let" Ident [ ":" Type ] "=" Expr ";" ;
MutDecl          = "mut" Ident ":" Type [ "=" Expr ] ";" ;
InferMutDecl     = Ident ":=" Expr ";" ;

FuncDecl         = OptVisibility "func" Ident
                   GenericParams?
                   "(" ParamList? ")"
                   ReturnType?
                   FuncBody ;

ReturnType       = "->" Type ;
FuncBody         = Block | "=>" Expr ";" ;

ParamList        = Param { "," Param } ;
Param            = Ident ":" Type ;
```

## 5. Structs, Classes, Traits, Impls

```ebnf
StructDecl       = OptVisibility "struct" Ident GenericParams? StructBody ;
StructBody       = "{" { StructMember } "}" ;
StructMember     = FieldDecl ;

ClassDecl        = OptVisibility "class" Ident GenericParams? ClassBody ;
ClassBody        = "{" { ClassMember } "}" ;
ClassMember      = ClassFieldDecl | MethodDecl ;

FieldDecl        = Ident ":" Type ";" ;
ClassFieldDecl   = MemberModifier FieldBinding Ident ":" Type
                   [ "=" Expr ] ";" ;

MethodDecl       = MethodModifier "func" Ident
                   GenericParams?
                   "(" ReceiverOrParamList? ")"
                   ReturnType?
                   FuncBody ;

ReceiverOrParamList = Receiver [ "," ParamList ] | ParamList ;
Receiver         = "self" ":" ( "&" | "&mut" ) Type ;

TraitDecl        = OptVisibility "trait" Ident GenericParams? TraitBody ;
TraitBody        = "{" { TraitItem } "}" ;
TraitItem        = [ "func" ] Ident GenericParams?
                   "(" ParamList? ")"
                   ReturnType?
                   ";" ;

ImplDecl         = "impl" GenericParams? Type [ "for" Type ]
                   "{" { MethodDecl } "}" ;
```

## 6. Generics and Constraints

```ebnf
GenericParams    = "<" GenericParam { "," GenericParam } ">" ;
GenericParam     = Ident [ ":" TypeBoundList ] ;
TypeBoundList    = TypeBound { "+" TypeBound } ;
TypeBound        = QualifiedIdent [ TypeArgs ] ;

TypeArgs         = "<" Type { "," Type } ">" ;
```

## 7. Types

```ebnf
Type             = RefType | PrimaryType ;
RefType          = "&" [ "mut" ] Type ;

PrimaryType      = NamedType
                 | ArrayType
                 | SliceType
                 | TupleType
                 | LambdaType ;

NamedType        = QualifiedIdent [ TypeArgs ] ;
ArrayType        = "[" Type ";" Expr "]" ;
SliceType        = "[" Type "]" ;
TupleType        = "(" Type { "," Type } [ "," ] ")" ;
LambdaType       = "lambda" "(" [ Type { "," Type } ] ")" "->" Type ;
```

## 8. Statements and Blocks

```ebnf
Block            = "{" { Stmt } "}" ;

Stmt             = LetDecl
                 | MutDecl
                 | InferMutDecl
                 | ConstDecl
                 | ExprStmt
                 | ReturnStmt
                 | ThrowStmt
                 | IfStmt
                 | WhileStmt
                 | ForStmt
                 | LoopStmt
                 | MatchStmt
                 | TryStmt ;

ExprStmt         = Expr ";" ;
ReturnStmt       = "return" [ Expr ] ";" ;
ThrowStmt        = "throw" Expr ";" ;

IfStmt           = "if" Expr Block [ "else" ( IfStmt | Block ) ] ;
WhileStmt        = "while" Expr Block ;
ForStmt          = "for" Ident "in" Expr Block ;
LoopStmt         = "loop" Block ;
TryStmt          = "try" Block { CatchClause } [ FinallyClause ] ;
CatchClause      = "catch" "(" Ident ":" Type ")" Block ;
FinallyClause    = "finally" Block ;
```

## 9. Match and Patterns

```ebnf
MatchStmt        = "match" Expr "{" MatchArm { MatchArm } "}" ;
MatchArm         = Pattern [ "if" Expr ] "=>" ( Expr ";" | Block ) ;

Pattern          = "_"
                 | Literal
                 | Ident
                 | RangePattern
                 | TuplePattern
                 | StructPattern
                 | EnumPattern ;

RangePattern     = Literal ".." Literal ;
TuplePattern     = "(" Pattern { "," Pattern } [ "," ] ")" ;
StructPattern    = QualifiedIdent "{" PatFieldList? "}" ;
PatFieldList     = PatField { "," PatField } [ "," ] ;
PatField         = Ident [ ":" Pattern ] ;
EnumPattern      = QualifiedIdent [ "(" Pattern { "," Pattern } [ "," ] ")" ] ;
```

## 10. Expressions

```ebnf
Expr             = AssignExpr ;

AssignExpr       = LogicalOrExpr [ AssignOp AssignExpr ] ;
AssignOp         = "=" | "+=" | "-=" | "*=" | "/=" ;

LogicalOrExpr    = LogicalAndExpr { "||" LogicalAndExpr } ;
LogicalAndExpr   = EqualityExpr { "&&" EqualityExpr } ;
EqualityExpr     = RelExpr { ( "==" | "!=" ) RelExpr } ;
RelExpr          = AddExpr { ( "<" | "<=" | ">" | ">=" ) AddExpr } ;
AddExpr          = MulExpr { ( "+" | "-" ) MulExpr } ;
MulExpr          = UnaryExpr { ( "*" | "/" | "%" ) UnaryExpr } ;

UnaryExpr        = ( "!" | "-" | "&" | "*" ) UnaryExpr
                 | PostfixExpr ;

PostfixExpr      = PrimaryExpr { PostfixOp } ;
PostfixOp        = CallOp | IndexOp | SliceOp | FieldOp ;
CallOp           = "(" ArgList? ")" ;
IndexOp          = "[" Expr "]" ;
SliceOp          = "[" Expr? ".." Expr? "]" ;
FieldOp          = "." Ident ;

ArgList          = Expr { "," Expr } ;

PrimaryExpr      = Literal
                 | Ident
                 | QualifiedIdent
                 | "(" Expr ")"
                 | ArrayLiteral
                 | StructLiteral
                 | ClosureExpr
                 | MatchExpr ;

MatchExpr        = "match" Expr "{" MatchArm { MatchArm } "}" ;

ArrayLiteral     = "[" [ Expr { "," Expr } [ "," ] ] "]" ;
StructLiteral    = QualifiedIdent "{" InitFieldList? "}" ;
InitFieldList    = InitField { "," InitField } [ "," ] ;
InitField        = Ident ":" Expr ;

ClosureExpr      = "|" ClosureParams? "|" ClosureTail ;
ClosureParams    = ClosureParam { "," ClosureParam } ;
ClosureParam     = Ident [ ":" Type ] ;
ClosureTail      = ( "->" Type )? ( Block | "=>" Expr ) ;
```

## 11. Literals

```ebnf
Literal          = IntLit | FloatLit | BoolLit | CharLit | StringLit | NullLit ;
BoolLit          = "true" | "false" ;
NullLit          = "null" ;
```

Lexical constraints (semantic phase):
- integer suffixes allowed: `i8/i16/i32/i64/u8/u16/u32/u64/int`
- float suffixes allowed: `f32/f64/float`
- `int` and `float` are target-profile aliases resolved in type checking

## 12. Governance Constraints (Non-grammar)

- No implicit dynamic typing
- No syntax that implies hidden heap allocation
- No user-defined operator precedence or custom operator tokens in v1
- Trait-based operator overloading only

## 13. Parser Notes

- Preferred strategy: recursive descent + Pratt expression parser.
- `match` appears as both statement and expression; parser can unify under expression grammar then allow as statement.
- `Receiver` requires lookahead on `self :` in method parameter list.
- Legacy class field spellings like `field name: T;` and bare `name: T;` remain
  parser-compatible for migration, but they are not part of the normalized v1
  surface.
- `async` / `await` / `spawn` remain planned and should be rejected with a dedicated diagnostic until the parser grows productions for them.
- Closure literals parse, but first-class closure invocation is still incomplete in the current front end. Keep closure-call examples out of the canonical accepted surface for now.
