#!/bin/sh
# usage: run.sh guest.wasm < guest-input
# Runs a guest module on out/runtime.wasm (itself hosted by $RUN).
D=$(dirname "$0")
{ wc -c < "$1"; cat "$1"; cat; } | ${RUN:-wasmtime} "$D/out/runtime.wasm"
