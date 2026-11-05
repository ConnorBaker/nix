# shellcheck shell=bash
source common.sh
source ../common/init.sh

requireEnvironment
setupConfig
enableFeatures "ca-derivations"
[[ -n $(type -p sqlite3) ]] || skipTest "needs sqlite3"
execUnshare ./ca-conflict-inner.sh
