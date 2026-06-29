#!/bin/sh
# resolve_dump.sh -- resolve a mem_trace dump's frames to function names on the
# RUN machine (where the libraries sit at the paths recorded in the dump), with
# no python needed. Emits a "frame<TAB>function" map on stdout; copy it to the
# build machine and feed analyze_mem_trace.py with --symmap.
#
#   sh resolve_dump.sh snap.dump > symmap.txt
#   sh resolve_dump.sh snap.dump aarch64-linux-gnu-addr2line > symmap.txt
#
# Requirements on the run machine: sh, awk, sort, addr2line. Only works if the
# deployed libraries still have symbols (not stripped). If they are stripped you
# get '??' here -- resolve on the build machine instead, via
#   analyze_mem_trace.py --resolve --bin-dir <dir-with-unstripped-libs>
# where the same build's unstripped libraries live.
#
# Dump line layout: <K> <addr> <size> <weight> <flags> <frame> <frame> ...
# A frame is  <module-path>+0x<offset>.
set -u
DUMP="${1:?usage: resolve_dump.sh DUMP [addr2line]}"
A2L="${2:-addr2line}"

# one line per unique frame; resolve each against its own module.
awk '{ for (i = 6; i <= NF; i++) print $i }' "$DUMP" | sort -u | \
while IFS= read -r fr; do
  case "$fr" in
    *+0x*) mod=${fr%+0x*}; off=0x${fr##*+0x} ;;
    *)     printf '%s\t??\n' "$fr"; continue ;;
  esac
  name="??"
  if [ -f "$mod" ]; then
    n=$("$A2L" -f -C -e "$mod" "$off" 2>/dev/null | head -1)
    [ -n "$n" ] && name="$n"
  fi
  printf '%s\t%s\n' "$fr" "$name"
done
