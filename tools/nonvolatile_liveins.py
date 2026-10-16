"""Functions that read a non-volatile register they did not set (for non_volatile_as_local).

Usage: python nonvolatile_liveins.py [--toml] [--verbose]

With the manifest's non_volatile_as_local, r14-r31 / f14-f31 / v14-v31 / v64-v127
become C++ locals of each function instead of ctx fields. Code that reads a
caller's non-volatile register without setting it (XDK D3D tail fragments such
as `return *(r31 + 0x164)` on the caller's frame) then reads its own
uninitialized local and crashes. Codegen keeps such functions on ctx when they
are marked `share_registers = true` in default_functions.toml (callers copy
their locals to ctx around the call and take them back).

This reads aot/generated/default/*.cpp (generated without the option) and, per
function, finds the non-volatiles read where no path from the entry has
written them (flow-sensitive over labels / gotos / returns / switch tables). Saves are not
reads: stores whose address comes from r1 and the __save* helpers. A call to a
function with live-in registers makes them live-in here too unless they are
set on every path to the call (fixpoint over the call graph).

Reported per function: the live-in registers, whether it calls __save* /
__rest* helpers (then share_registers would skip its restores - review it), and
conditional tail calls into it (codegen does not copy locals for those).
"""
import collections
import glob
import os
import re
import sys

GEN = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "aot", "generated", "default")

RE_FUNC = re.compile(r"^DEFINE_REX_FUNC\((\w+)\)")
RE_LABEL = re.compile(r"^(loc_[0-9A-F]+):")
RE_GOTO = re.compile(r"goto (loc_[0-9A-F]+);")
RE_REG = re.compile(r"ctx\.([rfv])(\d+)\b")
RE_CALL = re.compile(r"^\s*(sub_[0-9A-F]{8}|__\w+)\(ctx, base\);")
RE_ASSIGN = re.compile(r"^\s*ctx\.([rfv])(\d+)\.(\w+)(?:\[\d+\])?\s*=[^=]")
RE_VSTORE = re.compile(r"simde_mm_store(\w*)\(\s*(?:\([\w\s\*]+\)\s*)?ctx\.v(\d+)\.")
RE_R1_DERIVED = re.compile(r"^\s*ctx\.r(\d+)\.\w+\s*=\s*ctx\.r1\.\w+\s*[+-]")


def nonvolatile(kind, n):
    if kind == "r" or kind == "f":
        return n >= 14
    return 14 <= n <= 31 or 64 <= n <= 127


def parse_functions():
    funcs = {}
    for path in sorted(glob.glob(os.path.join(GEN, "armyoftworecomp_recomp.*.cpp"))):
        cur = None
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                m = RE_FUNC.match(line)
                if m:
                    cur = []
                    funcs[m.group(1)] = cur
                    continue
                if cur is None:
                    continue
                if line.startswith("}"):
                    cur = None
                    continue
                s = line.rstrip("\n")
                if s.lstrip().startswith("//") or not s.strip():
                    continue
                cur.append(s)
    return funcs


class Stmt:
    __slots__ = ("reads", "writes", "calls", "succ", "save")


def analyze_body(lines):
    """Statements with reads / writes of non-volatiles, direct calls and successors."""
    labels = {}
    for i, s in enumerate(lines):
        m = RE_LABEL.match(s)
        if m:
            labels[m.group(1)] = i
    stmts = []
    r1_derived = {1}
    ea_stack = False  # `ea` holds an address computed from r1
    depth_stack = []  # (index of the "if (...) {" line)
    block_end = {}
    for i, s in enumerate(lines):
        st = s.strip()
        if st.endswith("{") and (st.startswith("if") or st.startswith("switch") or st.startswith("else")):
            depth_stack.append(i)
        elif st == "}" or st.startswith("}"):
            if depth_stack:
                block_end[depth_stack.pop()] = i
    for i, s in enumerate(lines):
        st = s.strip()
        stmt = Stmt()
        stmt.reads, stmt.writes, stmt.calls = set(), set(), []
        stmt.save = False
        succ = []
        call = RE_CALL.match(s)
        if call and (call.group(1).startswith("__save") or call.group(1).startswith("__rest")):
            stmt.save = True
        elif call:
            stmt.calls.append(call.group(1))
        # Writes: whole-register assignments; vector stores into ctx.vN.
        partial_vec = False
        m = RE_ASSIGN.match(s)
        lhs_end = 0
        if m and nonvolatile(m.group(1), int(m.group(2))):
            reg = (m.group(1), int(m.group(2)))
            lhs_end = m.end() - 1
            # A lane write (vlogefp & co. write all four, one by one) counts as
            # setting the register.
            stmt.writes.add(reg)
        vm = RE_VSTORE.search(s)
        if vm and nonvolatile("v", int(vm.group(2))):
            reg = ("v", int(vm.group(2)))
            if vm.group(1) in ("_ss", "_sd", "l_pi", "h_pi", "l_pd", "h_pd"):
                partial_vec = True
            else:
                stmt.writes.add(reg)
        # Reads: every other mention (the assignment's left side excluded).
        is_store = st.startswith("REX_STORE") or (st.startswith("simde_mm_store") and "REX_RAW_ADDR" in st)
        if st.startswith("ea = "):
            ea_regs = {int(x) for x in re.findall(r"ctx\.r(\d+)\.", st)}
            ea_stack = bool(ea_regs & r1_derived)
        rhs = s[lhs_end:] if lhs_end else s
        for km in RE_REG.finditer(rhs):
            reg = (km.group(1), int(km.group(2)))
            if not nonvolatile(*reg):
                continue
            if vm and reg == ("v", int(vm.group(2))) and km.start() == vm.start(2) - 5 - (0):
                pass
            stmt.reads.add(reg)
        if vm and not partial_vec:
            # The destination of a vector store is not read.
            dst = ("v", int(vm.group(2)))
            occurrences = len([1 for km in RE_REG.finditer(rhs) if (km.group(1), int(km.group(2))) == dst])
            if occurrences <= 1:
                stmt.reads.discard(dst)
        # A store of a non-volatile to a stack address is a save.
        if is_store:
            addr_regs = {int(x) for x in re.findall(r"ctx\.r(\d+)\.u32\s*\+", st)}
            via_ea = ea_stack and re.search(r"(REX_STORE_\w+\(ea|REX_RAW_ADDR\(ea\))", st)
            if via_ea or (addr_regs and addr_regs <= r1_derived):
                stmt.reads = {r for r in stmt.reads if r[0] == "r" and r[1] < 14}
                stmt.save = True
        d = RE_R1_DERIVED.match(s)
        if d:
            r1_derived.add(int(d.group(1)))
        elif m and m.group(1) == "r" and int(m.group(2)) in r1_derived and int(m.group(2)) != 1:
            r1_derived.discard(int(m.group(2)))
        # Successors.
        terminal = False
        for g in RE_GOTO.finditer(st):
            if g.group(1) in labels:
                succ.append(labels[g.group(1)])
        if st.startswith("goto ") or st == "return;" or st.startswith("REX_FATAL") or "__builtin_unreachable" in st:
            terminal = True
        if call and i + 1 < len(lines) and lines[i + 1].strip() == "return;":
            pass
        if i in block_end and st.startswith("if"):
            succ.append(block_end[i] + 1)
        if not terminal and i + 1 < len(lines):
            succ.append(i + 1)
        stmt.succ = succ
        stmts.append(stmt)
    return stmts


# setjmp (TU2 0x82E2F700): reads every non-volatile only to store it.
SAVES_ALL = {"sub_82E2F700"}

ALL_NV = frozenset([("r", n) for n in range(14, 32)] + [("f", n) for n in range(14, 32)] +
                   [("v", n) for n in list(range(14, 32)) + list(range(64, 128))])


def live_ins(stmts, callee_liveins):
    """Non-volatiles read (directly or by a callee) where no path has set them.

    "May be set" (union over predecessors), not "set on every path": a read on
    a path the CFG allows but the code never takes (set in one branch, read
    after the join) reads the zero-initialized local harmlessly, while a tail
    fragment reads its caller's register before anything could have set it.
    """
    n = len(stmts)
    if not n:
        return set()
    defined_in = [None] * n  # may-be-defined at entry of each statement
    defined_in[0] = frozenset()
    work = collections.deque([0])
    while work:
        i = work.popleft()
        out = defined_in[i] | stmts[i].writes
        for j in stmts[i].succ:
            if j >= n:
                continue
            new = out if defined_in[j] is None else defined_in[j] | out
            if new != defined_in[j]:
                defined_in[j] = frozenset(new)
                work.append(j)
    result = set()
    for i, st in enumerate(stmts):
        if defined_in[i] is None:
            continue  # unreachable
        result |= st.reads - defined_in[i]
        for c in st.calls:
            result |= callee_liveins.get(c, set()) - defined_in[i]
    return result


def main():
    verbose = "--verbose" in sys.argv
    funcs = parse_functions()
    bodies = {name: analyze_body(lines) for name, lines in funcs.items()}
    callers = collections.defaultdict(set)
    cond_tail = collections.defaultdict(set)
    for name, lines in funcs.items():
        for i, s in enumerate(lines):
            m = RE_CALL.match(s)
            if m:
                callers[m.group(1)].add(name)
                if s.startswith("\t\t") and i > 0 and lines[i - 1].strip().startswith("if"):
                    cond_tail[m.group(1)].add(name)
    # setjmp stores every non-volatile into the jmp_buf: saves, not uses.
    for name in SAVES_ALL:
        if name in bodies:
            for st in bodies[name]:
                st.reads = set()
    liveins = {}
    changed = True
    rounds = 0
    while changed:
        changed = False
        rounds += 1
        for name, stmts in bodies.items():
            li = live_ins(stmts, liveins)
            if li != liveins.get(name, set()):
                liveins[name] = li
                changed = True
    flagged = {k: v for k, v in liveins.items() if v}
    direct = {k: live_ins(bodies[k], {}) for k in flagged}
    if "--direct" in sys.argv:
        for name in sorted(direct):
            if direct[name]:
                print(f"{name}: {' '.join(f'{a}{b}' for a, b in sorted(direct[name]))}")
        return
    saves = {name for name, lines in funcs.items()
             if any(re.match(r"^\s*__(save|rest)", s) for s in lines)}

    def fmt(regs):
        return " ".join(f"{k}{n}" for k, n in sorted(regs))

    print(f"# {len(funcs)} functions, {rounds} rounds, {len(flagged)} with live-in non-volatiles")
    fv = [k for k, v in flagged.items() if any(r[0] != "r" for r in v)]
    print(f"# reading f14+/v14+: {len(fv)}; with __save/__rest helpers: {len(set(flagged) & saves)}; "
          f"entered by a conditional tail call: {len([k for k in flagged if cond_tail.get(k)])}")
    if "--toml" in sys.argv:
        # The save / restore helpers are codegen's own business.
        for name in sorted(n for n in flagged if n.startswith("sub_")):
            print(f"0x{name[4:]} = {{ share_registers = true }}  # {fmt(flagged[name])}")
        return
    for name in sorted(flagged):
        notes = []
        if name in saves:
            notes.append("SAVE/REST")
        if cond_tail.get(name):
            notes.append("cond-tail from " + ",".join(sorted(cond_tail[name])[:3]))
        print(f"{name}: {fmt(flagged[name])}  callers={len(callers.get(name, ()))} {' '.join(notes)}")
        if verbose:
            print("   callers:", " ".join(sorted(callers.get(name, ()))[:8]))


if __name__ == "__main__":
    main()
