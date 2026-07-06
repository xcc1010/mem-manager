#!/bin/sh
# resolve_dump_nm.sh -- resolve a mem_trace dump's frames to function names on
# the RUN machine using ONLY the ELF symbol table (nm). No addr2line, no DWARF,
# no -g required. Emits "frame<TAB>function" on stdout; copy it to the build
# machine and feed analyze_mem_trace.py with --symmap.
#
#   sh resolve_dump_nm.sh snap.dump > symmap.txt
#   sh resolve_dump_nm.sh snap.dump aarch64-linux-gnu-nm aarch64-linux-gnu-readelf > symmap.txt
#
# Why no -g is needed: -g adds DWARF (file:line + inlined frames). FUNCTION
# NAMES come from the ELF symbol table, which a normal (unstripped) build keeps.
# Requirement: the module still has symbols. `nm` uses .symtab (all functions,
# incl. static); if that was stripped it falls back to .dynsym (exported only).
# Fully stripped (no .symtab and no .dynsym) -> '??'; resolve on the build
# machine instead (analyze_mem_trace.py --resolve --bin-dir <unstripped libs>).
#
# Dump line layout: <K> <addr> <size> <weight> [<resident>] <flags> <frame...>
# A frame is <module-path>+0x<offset>, offset = pc - dli_fbase (module-relative).
set -u
DUMP="${1:?usage: resolve_dump_nm.sh DUMP [nm] [readelf]}"
NM="${2:-nm}"
READELF="${3:-readelf}"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
MODS="$WORK/mods"; SYMS="$WORK/syms"; FRAMES="$WORK/frames"

# pure-awk hex->decimal (busybox/mawk/gawk safe; no strtonum). Offsets are
# module-relative (< a few hundred MB) so they fit awk's double exactly.
H2D='function h(s,  i,c,v){s=tolower(s);v=0;for(i=1;i<=length(s);i++){c=index("0123456789abcdef",substr(s,i,1));if(!c)return -1;v=v*16+c-1}return v}'

# unique frames, and the distinct modules they reference
awk '{for(i=6;i<=NF;i++) print $i}' "$DUMP" | sort -u > "$FRAMES"
sed -n 's/+0x[0-9a-fA-F]*$//p' "$FRAMES" | sort -u > "$MODS"

# per module: reladdr = nm_addr - first_LOAD_vaddr  (0 for .so/PIE, ~0x400000
# for a non-PIE exe) so nm addresses line up with the dump's relative offsets.
: > "$SYMS"
while IFS= read -r mod; do
    [ -f "$mod" ] || { echo "WARN: cannot find $mod on this machine" >&2; continue; }
    lv=$("$READELF" -lW "$mod" 2>/dev/null | awk 'toupper($1)=="LOAD"{print $3; exit}')
    [ -n "$lv" ] || lv=0x0
    s=$("$NM" -nC --defined-only "$mod" 2>/dev/null)          # .symtab (all funcs)
    [ -n "$s" ] || s=$("$NM" -DnC --defined-only "$mod" 2>/dev/null)  # fallback .dynsym
    [ -n "$s" ] || echo "WARN: no symbols in $mod (stripped?)" >&2
    printf '%s\n' "$s" | awk -v m="$mod" -v lv="$lv" "$H2D"'
        NF>=3 && $1 ~ /^[0-9a-fA-F]+$/ {
            nm=$3; for(k=4;k<=NF;k++) nm=nm" "$k
            printf "%s\t%.0f\t%s\n", m, h($1)-h(lv), nm
        }' >> "$SYMS"
done < "$MODS"

# resolve each frame: greatest symbol addr <= the frame offset, same module.
awk -F'\t' "$H2D"'
    FNR==NR { c=++cnt[$1]; A[$1 SUBSEP c]=$2+0; N[$1 SUBSEP c]=$3; tot[$1]=c; next }
    {
        fr=$0; s=index(fr,"+0x")
        if (s==0) { print fr "\t??"; next }
        m=substr(fr,1,s-1); o=h(substr(fr,s+3))
        best=-1; bn="??"
        for (i=1; i<=tot[m]; i++) { a=A[m SUBSEP i]; if (a<=o && a>best) { best=a; bn=N[m SUBSEP i] } }
        print fr "\t" bn
    }
' "$SYMS" "$FRAMES"
