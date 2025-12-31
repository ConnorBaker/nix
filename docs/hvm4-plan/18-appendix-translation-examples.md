# Appendix: Nix-to-HVM4 Translation Examples

This section shows how common Nix patterns translate to HVM4 terms, useful for understanding the compilation process.

Status (2025-12-28): Examples below reflect the current HVM4 encoding (#Ats/#Atr,
string table `#Str`, NUM booleans). String context, dynamic interpolation, and
structured errors are not implemented.

## Simple Values

```
Nix: 42
HVM4: 42  (raw NUM)

Nix: "hello"
HVM4: #Str{42}  (42 is a string-table id)

Nix: true
HVM4: 1  (NUM)

Nix: false
HVM4: 0  (NUM)

Nix: null
HVM4: #Nul{}
```

## Let Bindings

```
Nix:
  let x = 1; y = 2; in x + y

HVM4 (simplified):
  (λx. (λy. (OP2 ADD x y)) 2) 1

Or with @let syntax:
  @let x = 1;
  @let y = 2;
  (OP2 ADD x y)
```

## Attribute Sets

```
Nix:
  { a = 1; b = 2; }

HVM4:
  #Ats{#Con{#Atr{sym_a, 1}, #Con{#Atr{sym_b, 2}, #Nil{}}}}

Note: sym_a and sym_b are symbol IDs, sorted numerically
```

## Attribute Access

```
Nix:
  attrs.name

HVM4:
  @lookup(sym_name, attrs)

Expands to searching the sorted list:
  @lookup = λkey.λattrs. λ{
    #Ats: λlist. @lookup_list(key, list)  // returns ERA if missing
  }(attrs)
```

## Functions

```
Nix:
  x: x + 1

HVM4:
  λx. (OP2 ADD x 1)

Nix:
  { a, b ? 0 }: a + b

HVM4 (desugared):
  λ__arg.
    @let a = @select(__arg, sym_a);
    @let b = @if (@hasAttr(__arg, sym_b))
                 (@select(__arg, sym_b))
                 (0);
    (OP2 ADD a b)
```

## Lists

```
Nix:
  [1 2 3]

HVM4:
  #Lst{3, #Con{1, #Con{2, #Con{3, #Nil{}}}}}

Note: Length 3 is cached, elements are thunks
```

## Conditionals

```
Nix:
  if cond then a else b

HVM4:
  (MAT 0 b (λ_. a)) cond

// NUM 0 = false, nonzero = true
```

## Recursion (rec)

```
Nix:
  rec { a = b + 1; b = 10; }

HVM4 (acyclic, after topo-sort):
  @let b = 10;
  @let a = (OP2 ADD b 1);
  #Ats{#Con{#Atr{sym_a, a}, #Con{#Atr{sym_b, b}, #Nil{}}}}

Nix:
  rec { even = n: ...; odd = n: ...; }

HVM4:
  // Cyclic rec not implemented; falls back to standard evaluator
```

## With Expressions

```
Nix:
  with { x = 1; }; x

HVM4 (static resolution):
  @let $with = #Ats{#Con{#Atr{sym_x, 1}, #Nil{}}};
  @select($with, sym_x)

Nix (nested with fallback):
  let x = 1; in with { x = 2; }; x

HVM4:
  // Current backend only looks at the innermost with; no outer fallback.
  @let $with = #Ats{#Con{#Atr{sym_x, 2}, #Nil{}}};
  @select($with, sym_x)
```

## String Interpolation

```
Nix:
  "hello ${name}!"

HVM4:
  #SCat{
    #SCat{#Str{...}, name},  // planned runtime concat form
    #Str{...}
  }

Note: Only fully-constant concatenations are compiled today, and no string
context is tracked.
```

## Attribute Update

```
Nix:
  { a = 1; } // { b = 2; }

HVM4 (layer wrapping):
  #Ats{#Con{#Atr{sym_a, 1}, #Con{#Atr{sym_b, 2}, #Nil{}}}}

// mergeAttrs eagerly merges two sorted spines (no layering)
```

## Function Application

```
Nix:
  f x

HVM4:
  (APP f x)

Or simply:
  f(x)
```

## Paths

```
Nix:
  ./foo/bar.nix

HVM4:
  #Pth{0, 123}  // 123 is a string-table id for "/absolute/path/foo/bar.nix"
```

## Assertions

```
Nix:
  assert cond; value

HVM4:
  @if cond
      value
      ERA
```
