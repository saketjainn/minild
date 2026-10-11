#!/bin/sh
# minild test suite for Linux/macOS: cross-compiles the examples with
# x86_64-w64-mingw32-gcc and runs the produced .exe files with Wine.
#   sudo apt install gcc-mingw-w64-x86-64 wine64     (Ubuntu/Debian)
set -u
cd "$(dirname "$0")/.."
CC=${CC:-x86_64-w64-mingw32-gcc}
RUN=${RUN:-wine}
export WINEDEBUG=-all
[ -x ./minild ] || ./build.sh >/dev/null || exit 1
OUT=build/tests
mkdir -p "$OUT"
pass=0; fail=0
good() { echo "  PASS  $1"; pass=$((pass + 1)); }
bad()  { echo "  FAIL  $1"; fail=$((fail + 1)); }

for f in examples/*.c; do
  "$CC" -c -O1 -ffreestanding "$f" -o "$OUT/$(basename "${f%.c}").o" || { echo "could not compile $f"; exit 1; }
done

run() {  # run NAME EXPECTED_EXIT "EXPECTED TEXT" objects...
  name=$1; want=$2; text=$3; shift 3
  objs=""; for o in "$@"; do objs="$objs $OUT/$o.o"; done
  if ! ./minild link $objs -o "$OUT/$name.exe" >/dev/null 2>"$OUT/$name.err"; then bad "$name: link failed"; cat "$OUT/$name.err"; return; fi
  $RUN "$OUT/$name.exe" >"$OUT/$name.txt" 2>/dev/null; code=$?
  [ "$code" = "$want" ] || { bad "$name: exit code $code, expected $want"; return; }
  [ -z "$text" ] || grep -qF "$text" "$OUT/$name.txt" || { bad "$name: output missing '$text'"; return; }
  good "$name: links, runs, exit code $code"
}
fails() {  # fails NAME "EXPECTED MESSAGE" objects...
  name=$1; msg=$2; shift 2
  objs=""; for o in "$@"; do objs="$objs $OUT/$o.o"; done
  if ./minild link $objs -o "$OUT/$name.exe" >/dev/null 2>"$OUT/$name.err"; then bad "$name: link should have failed"; return; fi
  grep -qF "$msg" "$OUT/$name.err" && good "$name: rejected with: $msg" || { bad "$name: wrong message"; cat "$OUT/$name.err"; }
}

run hello 0 "Hello from a program linked by minild" start hello print
run calc  7 ""                                      start main math
run data  0 "all checks passed, total = 42"         start data_main data_lib print
run stub  0 "stub call through the IAT works"       start stub print
run weak  0 "strong greeting"                       start weak_main weak_default weak_strong print
run weak2 0 "weak default greeting"                 start weak_main weak_default print
fails undefined "undefined symbol 'add'"              start main
fails duplicate "duplicate symbol 'add'"              start main math dup
fails noentry   "entry symbol 'start' is not defined" main math

for o in main data_main start; do
  mine=$(./minild inspect "$OUT/$o.o" | grep -c IMAGE_REL_AMD64)
  theirs=$(x86_64-w64-mingw32-objdump -r "$OUT/$o.o" | grep -c IMAGE_REL_AMD64)
  [ "$mine" = "$theirs" ] && [ "$mine" != 0 ] && good "inspect $o.o: $mine relocations, same as objdump" || bad "inspect $o.o: $mine vs objdump $theirs"
done

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
