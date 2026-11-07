# shellcheck shell=bash
source common.sh
source ../common/init.sh

requireEnvironment
setupConfig
enableFeatures "ca-derivations"
execUnshare ./ca-build-traces-inner.sh
