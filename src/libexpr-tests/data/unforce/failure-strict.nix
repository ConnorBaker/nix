# Nothing is printed when forcing fails part-way (`--strict`).
builtins.genList (x: x + "foo") 2
