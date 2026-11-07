#include "nix/store/build/derivation-builder.hh"

/* The build child of a program that does not call buildChildMain(). */
int main(int argc, char ** argv)
{
    nix::buildChildMain(argc, argv);
    return 1;
}
