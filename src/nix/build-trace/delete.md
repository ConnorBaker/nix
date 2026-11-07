R"MdBoundary(
# Description

Delete build traces from the store.

Each id names its outputs, as `<drv>^<output>` or `<drv>^<output>,<output>`; `<drv>^*` (all outputs) is refused.

# Examples

Delete a build trace by its ID:

```console
$ nix store build-trace delete /nix/store/wfyy9qiwph8zxk68g4p71nrxi0k59x5y-python3-minimal-3.13.12.drv^debug
```

)MdBoundary"
