#!/bin/sh
#
# build-filc.sh - build the modular simso (src/) with Fil-C.
#
# Companion to build.sh, which builds the legacy single-file simso.c. This
# one compiles the five translation units under src/ and links them into one
# binary.
#
# Fil-C (https://fil-c.org) is a memory-safe C implementation: it enforces
# bounds and use-after-free checks at runtime and aborts with a `filc panic`
# rather than corrupting memory. Running the simulator under it is a standing
# check on the hardening work -- the DOS original would almost certainly have
# tripped it on the first run.
#
#   ./build-filc.sh                  release-ish build with debug info
#   ./build-filc.sh --debug          -O0, full debug
#   ./build-filc.sh --release        -O2, stripped (no Fil-C backtraces)
#   ./build-filc.sh -o simso-filc    choose the output name
#   ./build-filc.sh --clean          remove objects and binary
#   ./build-filc.sh --run            build, then run it
#   CC=gcc ./build-filc.sh           build with something other than Fil-C
#
set -eu

cd "$(dirname "$0")"

SRC="src/sim.c src/shared.c src/ui.c src/bench.c src/main.c"
OUT=simso
BUILD=build/filc
OPT="-O2 -g"
STRIP=no
RUN=no
VERBOSE=no

say()  { printf '%s\n' "$*"; }
die()  { printf 'build-filc.sh: %s\n' "$*" >&2; exit 1; }
run()  { [ "$VERBOSE" = yes ] && printf '  %s\n' "$*"; eval "$@"; }

# ---------------------------------------------------------------- options --
while [ $# -gt 0 ]; do
    case "$1" in
        --debug)    OPT="-O0 -g3 -fno-omit-frame-pointer" ;;
        --release)  OPT="-O2"; STRIP=yes ;;
        --run)      RUN=yes ;;
        -v|--verbose) VERBOSE=yes ;;
        -o)         [ $# -ge 2 ] || die "-o needs a name"; OUT=$2; shift ;;
        --clean)    rm -rf "$BUILD" "$OUT"; say "cleaned"; exit 0 ;;
        -h|--help)  sed -n '3,22p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)          die "unknown option '$1' (try --help)" ;;
    esac
    shift
done

# ------------------------------------------------------------- find filcc --
# Honour $CC if the caller set it, otherwise look for filcc on PATH and then
# in the places a Fil-C install usually unpacks to.
if [ "${CC:-}" = "" ]; then
    if command -v filcc >/dev/null 2>&1; then
        CC=filcc
    else
        for d in /opt/fil*/bin /usr/local/fil*/bin "$HOME"/fil*/bin \
                 "$HOME"/.local/fil*/bin /opt/filc/bin; do
            [ -x "$d/filcc" ] && { CC="$d/filcc"; break; }
        done
    fi
fi

[ "${CC:-}" = "" ] && die "filcc not found.

Install Fil-C from https://fil-c.org and put its bin/ on your PATH, or point
at it directly:

    CC=/path/to/filcc ./build-filc.sh

To build with an ordinary compiler instead (no memory-safety checking):

    CC=gcc ./build-filc.sh"

command -v "$CC" >/dev/null 2>&1 || [ -x "$CC" ] || die "compiler '$CC' is not executable"

case "$CC" in
    *filcc) SAFE=yes ;;
    *)      SAFE=no  ;;
esac

# ------------------------------------------------------- probe for ncurses --
# The dashboard draws Unicode block glyphs, which only the wide-character
# build (ncursesw) measures correctly. Prefer it; fall back to the narrow
# build with ASCII glyphs forced on rather than rendering garbage.
probe_dir=$(mktemp -d) || die "mktemp failed"
trap 'rm -rf "$probe_dir"' EXIT INT TERM

cat > "$probe_dir/probe.c" <<'EOF'
#include <ncurses.h>
int main(void) { return initscr() ? 0 : 1; }
EOF

NCURSES=
EXTRA_DEFS=
for lib in ncursesw ncurses; do
    if "$CC" $OPT -o "$probe_dir/probe" "$probe_dir/probe.c" \
             "-l$lib" >/dev/null 2>&1; then
        NCURSES="-l$lib"
        [ "$lib" = ncurses ] && EXTRA_DEFS="-DSIMSO_FORCE_ASCII=1"
        break
    fi
done

[ "$NCURSES" = "" ] && die "no usable ncurses found.

Tried -lncursesw and -lncurses with '$CC'.
  - with Fil-C, ncurses ships in the pizfix; check what yours provides:
        ls \$(dirname \$(command -v filcc))/../*/lib | grep -i ncurses
  - with a system compiler, install the headers:
        sudo apt-get install libncursesw5-dev    # Debian/Ubuntu
        sudo pacman -S ncurses                   # Arch"

# --------------------------------------------------------------- warnings --
# Fil-C's frontend does not accept every GCC warning switch, so keep only the
# ones it takes.
WARN=
for w in -Wall -Wextra -Wshadow -Wpointer-arith -Wno-unused-parameter; do
    if "$CC" $w -fsyntax-only "$probe_dir/probe.c" >/dev/null 2>&1; then
        WARN="$WARN $w"
    fi
done

CFLAGS="-std=gnu11 $WARN $OPT -pthread $EXTRA_DEFS"
LDFLAGS="-pthread"

say "simso build"
say "  compiler   $CC$([ "$SAFE" = yes ] && echo '  (Fil-C: memory-safe)')"
say "  ncurses    $NCURSES$([ -n "$EXTRA_DEFS" ] && echo '  (narrow build -> ASCII glyphs)')"
say "  flags      $OPT"
say "  output     $OUT"
say ""

# ----------------------------------------------------------------- compile --
mkdir -p "$BUILD"

# Objects are only reusable if they were built with the same compiler and
# flags, which timestamps cannot tell us -- so stamp them. Without this,
# `--release` after `--debug` would silently relink the debug objects.
stamp="$BUILD/.buildflags"
want="$CC|$CFLAGS|$LDFLAGS|$NCURSES"
if [ ! -f "$stamp" ] || [ "$(cat "$stamp")" != "$want" ]; then
    [ -f "$stamp" ] && say "  (flags changed, rebuilding everything)"
    rm -f "$BUILD"/*.o
    printf '%s' "$want" > "$stamp"
fi

objs=
for c in $SRC; do
    o="$BUILD/$(basename "${c%.c}").o"
    objs="$objs $o"

    # rebuild when the source, or any header, is newer than the object
    stale=no
    if [ ! -f "$o" ]; then
        stale=yes
    elif [ "$c" -nt "$o" ]; then
        stale=yes
    else
        for h in src/*.h; do
            [ "$h" -nt "$o" ] && { stale=yes; break; }
        done
    fi

    if [ "$stale" = no ]; then
        say "  up to date $c"
        continue
    fi

    say "  compiling  $c"
    run "\"$CC\" $CFLAGS -Isrc -c \"$c\" -o \"$o\"" \
        || die "compilation of $c failed"
done

# relink only when an object is newer than the binary
relink=yes
if [ -f "$OUT" ]; then
    relink=no
    for o in $objs; do
        [ "$o" -nt "$OUT" ] && { relink=yes; break; }
    done
fi

if [ "$relink" = yes ]; then
    say "  linking    $OUT"
    run "\"$CC\" $LDFLAGS -o \"$OUT\" $objs $NCURSES" || die "link failed"
    [ "$STRIP" = yes ] && command -v strip >/dev/null 2>&1 && strip "$OUT"
else
    say "  up to date $OUT"
fi

say ""
say "built $OUT ($(wc -c < "$OUT" | tr -d ' ') bytes)"
if [ "$SAFE" = yes ]; then
    say ""
    say "Fil-C notes:"
    say "  - a bounds or use-after-free violation aborts with 'filc panic'"
    say "    plus a backtrace; keep -g (the default here) for line numbers"
    say "  - exercise the scheduler hard without a terminal:"
    say "        ./$OUT --bench 5000 --all"
fi

[ "$RUN" = yes ] && { say ""; exec "./$OUT"; }
exit 0
