# shellcheck shell=bash
source common.sh
source ../common/init.sh

requireEnvironment
type -p sqlite3 > /dev/null || skipTest "sqlite3 is not on PATH"
setupConfig
execUnshare ./gc-objects-inner.sh
