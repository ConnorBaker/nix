#!/bin/bash
gdb -batch -ex "break value.hh:542" -ex "run eval --file gnomes.nix" -ex "print/x p0_" -ex "print/x pd" -ex "print this" -ex "bt 10" ./build/src/nix/nix 2>&1 | tail -50
