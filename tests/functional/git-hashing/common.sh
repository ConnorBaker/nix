# shellcheck shell=bash

source ../common.sh

TODO_NixOS # Need to make sure test is ok for store we don't clear

# Need backend to support the git content-address method too
requireDaemonNewerThan "2.19"
