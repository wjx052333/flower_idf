#!/bin/sh
# Host-side tests for water_logic.c.
# Needs a C compiler with POSIX setenv()/localtime_r(): MinGW-w64, gcc or clang.
set -e
cd "$(dirname "$0")"

OUT=test_water
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) OUT=test_water.exe ;;
esac

CC=${CC:-gcc}
echo "compiling with $CC"
"$CC" -std=gnu99 -Wall -Wextra -O1 -I../main -o "$OUT" test_water.c ../main/water_logic.c

echo "running tests"
"./$OUT"
