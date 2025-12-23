# Appendix: Nix-to-HVM4 Translation Examples

This section shows how common Nix patterns translate to HVM4 terms, useful for understanding the compilation process.

## Simple Values

```
Nix: 42
HVM4: 42  (raw NUM)

Nix: "hello"
HVM4: #Str{#Con{#Chr{104}, #Con{#Chr{101}, #Con{#Chr{108}, #Con{#Chr{108}, #Con{#Chr{111}, #Nil{}}}}}}, #NoC{}}

Nix: true
HVM4: #Tru{}

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
  #ABs{#Con{#Atr{sym_a, 1}, #Con{#Atr{sym_b, 2}, #Nil{}}}}

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
    #ABs: λlist. @lookup_list(key, list)
    #ALy: λoverlay.λbase.
      @lookup_list(key, overlay) .or. @lookup(key, base)
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
  λ{#Tru: a; #Fls: b}(cond)

Or using MAT:
  (MAT cond #Tru a #Fls b)
```

## Recursion (rec)

```
Nix:
  rec { a = b + 1; b = 10; }

HVM4 (acyclic, after topo-sort):
  @let b = 10;
  @let a = (OP2 ADD b 1);
  #ABs{#Con{#Atr{sym_a, a}, #Con{#Atr{sym_b, b}, #Nil{}}}}

Nix:
  rec { even = n: ...; odd = n: ...; }

HVM4 (cyclic, using Y-combinator):
  @Y(λself. #ABs{
    #Atr{sym_even, λn. ... (@select(self, sym_odd)) ...},
    #Atr{sym_odd, λn. ... (@select(self, sym_even)) ...}
  })
```

## With Expressions

```
Nix:
  with { x = 1; }; x

HVM4 (static resolution):
  @let $with = #ABs{#Con{#Atr{sym_x, 1}, #Nil{}}};
  @select($with, sym_x)

Nix (ambiguous):
  let x = 1; in with { x = 2; }; x

HVM4:
  @let x = 1;
  @let $with = #ABs{#Con{#Atr{sym_x, 2}, #Nil{}}};
  @if (@hasAttr($with, sym_x))
      (@select($with, sym_x))
      (x)
```

## String Interpolation

```
Nix:
  "hello ${name}!"

HVM4:
  @str_concat(
    @str_concat(
      #Str{[h,e,l,l,o, ], #NoC{}},
      @coerce_to_string(name)
    ),
    #Str{[!], #NoC{}}
  )

Note: Context from 'name' is merged into result
```

## Attribute Update

```
Nix:
  { a = 1; } // { b = 2; }

HVM4 (layer wrapping):
  #ALy{
    #Con{#Atr{sym_b, 2}, #Nil{}},  // overlay
    #ABs{#Con{#Atr{sym_a, 1}, #Nil{}}}  // base
  }
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
  #Pth{0, #Str{"/absolute/path/foo/bar.nix", #NoC{}}}
```

## Assertions

```
Nix:
  assert cond; value

HVM4:
  @if cond
      value
      (#Err{@make_string("assertion failed")})
```
