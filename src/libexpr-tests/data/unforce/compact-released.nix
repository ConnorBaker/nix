# Each element an integer computed from garbage: its job keeps nothing,
# and leaves all its memory to the next.
builtins.genList (i: builtins.length (builtins.genList (k: [ (k + i) ]) 20000)) 8
