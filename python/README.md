# DSL Compiler (Python)

A deterministic, dependency‑free compiler for a small schema definition language (DSL) that generates C++17 headers. The generated code is allocation‑free, uses `std::string_view` and `ListView<T>`, and follows strict naming conventions:

- **Types:** PascalCase  
- **Enum values:** snake_case  
- **Fields:** snake_case  
- **Macros:** ALL_CAPS (rare, intentional)

The compiler is structured into four explicit layers:

- **Lexer** — converts raw text into tokens  
- **Parser** — builds an abstract syntax tree (AST)  
- **Validator** — enforces semantic rules (types, cycles, duplicates, etc.)  
- **Generator** — emits C++17 code from the AST  

Everything is pure Python and uses only the standard library.

---

## Directory Structure

