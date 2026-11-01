# `//` chains of every shape (layers shared, merged, a layered right operand), read by every attribute-set operation.
let
  inherit (builtins) genList foldl' listToAttrs attrNames attrValues mapAttrs intersectAttrs removeAttrs;
  h = x: builtins.hashString "sha256" (builtins.toJSON x);
  mk = p: n: listToAttrs (genList (i: { name = "${p}${toString i}"; value = i; }) n);
  big = mk "a" 1000;
  # Plain right operands over a large base: shared layers within the depth bound.
  shared = foldl' (acc: i: acc // { "a${toString i}" = -i; "n${toString i}" = i; }) big (genList (i: i * 37) 20);
  # Right operands as large as the chain, overriding c0: compactions.
  compacted = foldl' (acc: i: acc // mk "c${toString i}-" (8 * i + 1) // { c0 = i; }) (mk "c" 2) (genList (i: i) 12);
  # A layered right operand (bound to a name: `a // (b // c)` is evaluated as one chain): copied, all its layers.
  rhs = mk "z" 3 // { z0 = "rhs"; };
  layeredRhs = big // rhs;
  small = mk "s" 3 // { s9 = 9; };
  # Names spread through a large right operand's array, under it.
  under = builtins.seq big (listToAttrs (genList (i: { name = "a${toString (i * 199)}"; value = "under"; }) 6 ++ [ { name = "zz"; value = 0; } ]) // big);
  check = s: {
    names = h (attrNames s);
    size = builtins.length (attrNames s);
    values = h (attrValues s);
    has = [ (s ? a37) (s ? n37) (s ? c0) (s ? z2) (s ? nope) ];
    ors = [ (s.nope or "d") (s.a1 or "d") (s.z1 or "d") ];
    pos = builtins.unsafeGetAttrPos "a37" s;
    eqFlat = s == listToAttrs (map (n: { name = n; value = s.${n}; }) (attrNames s));
    neq = s == s // { a1 = "x"; };
    mapped = h (attrValues (mapAttrs (n: v: [ n v ]) s));
    removed = h (attrNames (removeAttrs s [ "a1" "n37" "c3-0" "z0" ]));
    inter = intersectAttrs { a37 = 0; n37 = 0; c0 = 0; c11-88 = 0; z0 = 0; z2 = 0; nope = 0; } s;
    inter2 = h (attrNames (intersectAttrs s (mk "a" 50 // mk "c5-" 50)));
  };
in
mapAttrs (_: check) { inherit shared compacted layeredRhs small under; }
// {
  rhsWins = [ shared.a37 shared.n37 layeredRhs.z0 compacted.c0 compacted.c11-0 under.a199 under.zz ];
  repeated = [ small small (small // { }) { inner = small; } ];
  posOfLayer = builtins.unsafeGetAttrPos "n37" shared;
}
