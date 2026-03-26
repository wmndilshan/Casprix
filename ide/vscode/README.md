# Casprix Language — VS Code Extension

Full language support for the [Casprix programming language](https://github.com/user/casprix) inside Visual Studio Code.

## Features

### 🎨 Syntax Highlighting
Complete TextMate grammar for `.cpx` and `.nd` files:
- Keywords, control flow, storage modifiers
- Built-in types and functions
- String literals with escape sequences and interpolation
- Numeric literals (dec, hex `0x`, binary `0b`, octal `0o`)
- Function definitions and calls
- Type annotations and generics
- Operators, brackets, and delimiters

### ✅ Error Highlighting (Diagnostics)
Errors and warnings are shown inline — sourced directly from the Casprix compiler:
- Runs `casprix --check-only` on **file save** (enabled by default)
- Optionally runs while **typing** (configurable, with debounce)
- Supports `error`, `warning`, `note`, and `hint` severity levels

### 💡 Auto-Complete
Press `Ctrl+Space` to get suggestions for:
- All language **keywords**
- All **built-in types** (`int`, `string`, `array`, `tensor`, …)
- All **built-in functions** (`print`, `len`, `assert`, `typeof`, …)

### 📝 Code Snippets
Type a prefix and press `Tab`:

| Prefix  | Inserts           |
|---------|-------------------|
| `func`  | Function skeleton |
| `funcr` | Function + return |
| `afunc` | Async function    |
| `class` | Class skeleton    |
| `struct`| Struct definition |
| `trait` | Trait definition  |
| `impl`  | Impl block        |
| `enum`  | Enum definition   |
| `let`   | Let binding       |
| `letm`  | Mutable binding   |
| `if`    | If statement      |
| `ife`   | If-else           |
| `for`   | For-in range loop |
| `each`  | For-each loop     |
| `while` | While loop        |
| `match` | Match expression  |
| `try`   | Try-catch block   |
| `print` | Print statement   |

### 📖 Hover Documentation
Hover over any keyword to see a brief description and usage example.

### 🔤 Smart Editing
- Auto-close brackets, parentheses, quotes, and block comments
- Bracket pair colorization
- Foldable `#region` / `#endregion` blocks
- Smart indentation on Enter

## Setup

### 1. Install the Extension
Install from the VSIX package:
```bash
cd ide/vscode
npm install
npx vsce package
# Then in VS Code: Extensions → ⋯ → Install from VSIX…
```

### 2. Configure the Compiler Path
The extension uses the Casprix compiler for error checking. Set the path in your workspace settings:

**`.vscode/settings.json`**:
```json
{
    "casprix.compilerPath": "D:/Projects/ND/build/casprix.exe"
}
```

Or add `build/` to your system `PATH` and leave the default `"casprix"`.

## Configuration

| Setting | Default | Description |
|---------|---------|-------------|
| `casprix.compilerPath` | `"casprix"` | Path to the compiler executable |
| `casprix.checkOnSave` | `true` | Run error check on file save |
| `casprix.checkOnType` | `false` | Run error check while typing |
| `casprix.checkDelay` | `800` | Typing debounce delay (ms) |

## Recommended VS Code Settings

```json
{
    "editor.formatOnSave": false,
    "editor.bracketPairColorization.enabled": true,
    "editor.guides.bracketPairs": "active"
}
```
