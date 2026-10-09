#!/bin/sh
# usage: lua/bundle.sh main.lua [module.lua ...]
# Whole-program compilation: put the modules in package.preload, ahead of
# the main program, so that require finds them.  A module's name is its
# file name without .lua.
main=$1; shift
for m in "$@"; do
  name=$(basename "$m" .lua)
  printf 'package.preload["%s"] = function(...)\n' "$name"
  sed '1{/^#!/d}' "$m"
  printf '\nend\n'
done
sed '1{/^#!/d}' "$main"
