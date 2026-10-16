"""Temporarily instruments a recompiled function with an entry log.

usage: instrument.py <cpp file> <func name> <condition> <label>
Logs r3..r6, lr when the C++ condition (on ctx) is true.
"""
import sys

path, func, cond, label = sys.argv[1:5]
s = open(path, encoding="utf-8").read()
old = "DEFINE_REX_FUNC(%s) {\n\tREX_FUNC_PROLOGUE();\n\tuint32_t ea{};" % func
assert old in s, "function not found"
new = old + (
    '\n\tif (%s) {\n'
    '\t\tFILE* dbg = fopen("vector_debug.log", "a");\n'
    '\t\tif (dbg) { fprintf(dbg, "%s r3=%%08X r4=%%08X r5=%%08X r6=%%08X lr=%%08X\\n", '
    'ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, (uint32_t)ctx.lr); fclose(dbg); }\n'
    '\t}' % (cond, label))
s = s.replace(old, new, 1)
if "#include <cstdio>" not in s:
    s = "#include <cstdio>\n" + s
open(path, "w", encoding="utf-8").write(s)
print("instrumented", func)
