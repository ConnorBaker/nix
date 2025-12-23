# Appendix: HVM4 Patterns Reference

> **Note:** This section shows generic HVM4 programming patterns. For the specific
> constructor names used in the Nix HVM4 encoding, see the [Overview](./00-overview.md).

## Pattern: Sum Types

```hvm4
// Option (Nix uses #Som/#Non)
#Non{}
#Som{value}

// Boolean (Nix uses #Tru/#Fls)
#Tru{}
#Fls{}

// Null
#Nul{}

// Either
#Lft{value}
#Rgt{value}

// Result
#Ok_{value}
#Err{error}
```

## Pattern: Recursive Data

```hvm4
// List (HVM4 native)
#Nil{}
#Con{head, tail}

// Tree
#Lef{value}
#Nod{left, right}

// With function
@length = λ{#Nil: 0; #Con: λh.λt. 1 + @length(t)}
```

## Pattern: Mutual Recursion

```hvm4
@even = λ{#Z: #Tru{}; #S: λn. @odd(n)}
@odd = λ{#Z: #Fls{}; #S: λn. @even(n)}
```

## Pattern: Effects via Continuation

```hvm4
// Effect type
#Pur{value}
#Eff{op, arg, continuation}

// Handler
@run = λ{
  #Pur: λv. v
  #Eff: λop.λarg.λk.
    // Handle op(arg), call k with result
}
```

## Pattern: State Threading

```hvm4
// State s a = s -> (a, s)
@return = λa. λs. #Par{a, s}
@bind = λma.λf. λs.
  let #Par{a, s'} = ma(s);
  f(a)(s')
```

## Pattern: Y-Combinator

```hvm4
// Fixed-point combinator for recursion
@Y = λf. (λx. f(x(x))) (λx. f(x(x)))

// Usage:
@factorial = @Y(λself. λn.
  (n == 0) .&. 1 .|. n * self(n - 1)
)
```

## Pattern: Conditional Evaluation

```hvm4
// Using pattern matching on boolean
@if = λcond.λthen.λelse. λ{
  #Tru: then
  #Fls: else
}(cond)

// Short-circuit AND
@and = λa.λb. λ{#Tru: b; #Fls: #Fls{}}(a)

// Short-circuit OR
@or = λa.λb. λ{#Tru: #Tru{}; #Fls: b}(a)
```

## Pattern: List Operations

```hvm4
@map = λf.λ{
  #Nil: #Nil{}
  #Con: λh.λt. #Con{f(h), @map(f, t)}
}

@filter = λp.λ{
  #Nil: #Nil{}
  #Con: λh.λt.
    p(h) .&. #Con{h, @filter(p, t)} .|. @filter(p, t)
}

@foldl = λf.λz.λ{
  #Nil: z
  #Con: λh.λt. @foldl(f, f(z, h), t)
}

@foldr = λf.λz.λ{
  #Nil: z
  #Con: λh.λt. f(h, @foldr(f, z, t))
}
```

## Pattern: Option Handling

```hvm4
@map_option = λf.λ{
  #Non: #Non{}
  #Som: λv. #Som{f(v)}
}

@or_default = λdefault.λ{
  #Non: default
  #Som: λv. v
}

@and_then = λf.λ{
  #Non: #Non{}
  #Som: λv. f(v)
}
```

## Pattern: Binary Search Tree

```hvm4
// BST = #Leaf{} | #Node{key, value, left, right}

@lookup = λkey.λ{
  #Leaf: #Non{}
  #Node: λk.λv.λl.λr.
    (key == k) .&. #Som{v} .|.
    (key < k) .&. @lookup(key, l) .|.
    @lookup(key, r)
}

@insert = λkey.λval.λ{
  #Leaf: #Node{key, val, #Leaf{}, #Leaf{}}
  #Node: λk.λv.λl.λr.
    (key == k) .&. #Node{key, val, l, r} .|.
    (key < k) .&. #Node{k, v, @insert(key, val, l), r} .|.
    #Node{k, v, l, @insert(key, val, r)}
}
```
