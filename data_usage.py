#!/home/daniel/.espressif/tools/python/v5.5.2/venv/bin/python
"""
data_usage.py — whole-program data variable usage analysis

Extends reachability.py's call-graph index with a per-function map of every
data variable (global, file-static, namespace-static, static member, or class
field) that each function reads or writes.

INDEX  (cached to .data_usage_index.json, built once):
  Parses every TU in compile_commands.json.
  Records the full caller→callee graph (same as reachability.py) PLUS a
  func_usr → {var_usr, …} access map and a var_usr → info dict.

QUERY  (instant from cache):
  BFS from the function at <file>:<line>.
  Outputs every reachable function together with the data variables it
  touches directly, then a de-duplicated summary across all reachable code.

Usage:
    python data_usage.py <file> <line>   # query (auto-builds index)
    python data_usage.py --index         # force-rebuild index only

Env vars:
    FILTER_SYSTEM=1   hide variables in .espressif/system headers (default 1)
    GROUP=func        group output by function (default)
    GROUP=var         group output by variable — which functions touch it
    VERBOSE=1         print per-TU progress during indexing
"""

import sys, os, re, shlex, json, subprocess, time
sys.path.insert(0, "/home/daniel/.espressif/tools/python/v5.5.2/venv/lib/"
                   "python3.12/site-packages")
import clang.cindex as cx

# ── constants ─────────────────────────────────────────────────────────────────

LIBCLANG   = ("/home/daniel/.espressif/tools/esp-clang-libs/"
              "esp-19.1.2_20250312/esp-clang/lib/libclang.so")
WORKSPACE  = os.path.dirname(os.path.abspath(__file__))
COMPILE_DB = os.path.join(WORKSPACE, "build", "compile_commands.json")
COMPILER   = ("/home/daniel/.espressif/tools/riscv32-esp-elf/"
              "esp-14.2.0_20251107/riscv32-esp-elf/bin/riscv32-esp-elf-g++")
CACHE_FILE = os.path.join(WORKSPACE, ".data_usage_index.json")

FILTER_SYSTEM = os.environ.get("FILTER_SYSTEM", "0") == "0"
VERBOSE       = os.environ.get("VERBOSE",       "0") == "1"
GROUP         = os.environ.get("GROUP",         "func")  # "func" | "var"

SYSTEM_PREFIXES = ("/home/daniel/.espressif/", "/usr/", "/opt/")

FUNC_KINDS = {
    cx.CursorKind.FUNCTION_DECL,
    cx.CursorKind.FUNCTION_TEMPLATE,
    cx.CursorKind.CXX_METHOD,
    cx.CursorKind.CONSTRUCTOR,
    cx.CursorKind.DESTRUCTOR,
    cx.CursorKind.CONVERSION_FUNCTION,
}

# ── VAR_DECL scope classification ─────────────────────────────────────────────
# We only track "interesting" variables — those that exist beyond a single
# function invocation.  Local variables (declared inside a function) are skipped.

_LOCAL_SCOPE_KINDS = {
    cx.CursorKind.FUNCTION_DECL,
    cx.CursorKind.FUNCTION_TEMPLATE,
    cx.CursorKind.CXX_METHOD,
    cx.CursorKind.CONSTRUCTOR,
    cx.CursorKind.DESTRUCTOR,
    cx.CursorKind.CONVERSION_FUNCTION,
}

# Parent kinds that make a VAR_DECL a global / static / namespace variable
_GLOBAL_SCOPE_KINDS = {
    cx.CursorKind.TRANSLATION_UNIT,
    cx.CursorKind.NAMESPACE,
    cx.CursorKind.CLASS_DECL,
    cx.CursorKind.STRUCT_DECL,
    cx.CursorKind.CLASS_TEMPLATE,
    cx.CursorKind.CLASS_TEMPLATE_PARTIAL_SPECIALIZATION,
    cx.CursorKind.UNION_DECL,
}

CALLBACK_TYPE_TOKENS = ("std::function", "function<")

# ── helpers ───────────────────────────────────────────────────────────────────

def vlog(*args):
    if VERBOSE:
        print(*args, file=sys.stderr)


def is_system(path):
    return not path or any(path.startswith(p) for p in SYSTEM_PREFIXES)


def fully_qualified(cursor):
    parts = []
    c = cursor
    while c and c.kind != cx.CursorKind.TRANSLATION_UNIT:
        if c.spelling:
            parts.append(c.spelling)
        if c.semantic_parent is None or c.semantic_parent == c:
            break
        c = c.semantic_parent
    return "::".join(reversed(parts))


def type_is_std_func(type_spelling):
    return any(t in type_spelling for t in CALLBACK_TYPE_TOKENS)


# ── compile DB + arg parsing ──────────────────────────────────────────────────

_compile_db = None

def get_compile_db():
    global _compile_db
    if _compile_db is None:
        with open(COMPILE_DB) as f:
            _compile_db = {e["file"]: e for e in json.load(f)}
    return _compile_db


def args_for_file(filepath):
    db    = get_compile_db()
    entry = db.get(filepath)
    if not entry:
        base = os.path.basename(filepath)
        for k, v in db.items():
            if os.path.basename(k) == base:
                entry = v; break
    if not entry:
        return []

    cmd  = entry.get("command", "")
    args = shlex.split(cmd) if cmd else entry.get("arguments", [])
    out  = []
    skip = False
    for tok in args[1:]:
        if skip:
            skip = False; continue
        if tok in ("-o", "-MF", "-MT", "-MQ"):
            skip = True; continue
        if tok.startswith(("-f", "-m", "-W", "-w", "-M", "-O", "-g", "-pipe")):
            continue
        if tok == filepath or tok.endswith((".c", ".cpp", ".cxx", ".cc")):
            continue
        out.append(tok)
    out.extend(_system_includes())
    return out


_sys_includes = None

def _system_includes():
    global _sys_includes
    if _sys_includes is not None:
        return _sys_includes
    try:
        r = subprocess.run(
            [COMPILER, "-x", "c++", "-v", "-E", "-"],
            input=b"", capture_output=True, timeout=15,
        )
        text = r.stderr.decode(errors="replace")
        collecting, inc = False, []
        for line in text.splitlines():
            if "#include <...>" in line:
                collecting = True; continue
            if collecting:
                if line.startswith("End of search list"):
                    break
                inc.append("-isystem" + line.strip())
        _sys_includes = inc
    except Exception:
        _sys_includes = []
    return _sys_includes


# ── libclang ──────────────────────────────────────────────────────────────────

cx.Config.set_library_file(LIBCLANG)
_index = cx.Index.create()


def get_overloaded_decls(cursor):
    """Enumerate candidates of an OVERLOADED_DECL_REF via raw libclang C API."""
    import ctypes
    lib = cx.conf.lib
    try:
        lib.clang_getNumOverloadedDecls.restype  = ctypes.c_uint
        lib.clang_getNumOverloadedDecls.argtypes = [cx.Cursor]
        lib.clang_getOverloadedDecl.restype      = cx.Cursor
        lib.clang_getOverloadedDecl.argtypes     = [cx.Cursor, ctypes.c_uint]
        n = lib.clang_getNumOverloadedDecls(cursor)
        return [lib.clang_getOverloadedDecl(cursor, i) for i in range(n)]
    except Exception:
        return []


def parse_tu(filepath):
    args = args_for_file(filepath)
    try:
        return _index.parse(
            filepath, args=args,
            options=(cx.TranslationUnit.PARSE_DETAILED_PROCESSING_RECORD |
                     cx.TranslationUnit.PARSE_INCOMPLETE),
        )
    except cx.TranslationUnitLoadError:
        return None


# ── single-TU indexing ────────────────────────────────────────────────────────

def index_tu(filepath, tu):
    """
    Walk one TU and return:
      func_info       : usr -> {name, file, line, system}
      edges           : caller_usr -> set of callee_usr
      var_bindings    : var_usr    -> set of func_usr    (stored callables)
      indirect_calls  : caller_usr -> set of var_usr
      method_sigs     : (spelling, nargs) -> set of usr
      func_var_access : func_usr  -> set of var_usr     ← NEW
      var_info        : var_usr   -> {name, type, parent, file, line, system} ← NEW
    """
    func_info      = {}
    edges          = {}
    var_bindings   = {}
    indirect_calls = {}
    method_sigs    = {}
    func_var_access = {}   # func_usr -> set of var_usr
    var_info        = {}   # var_usr  -> info dict

    func_stack = []  # USRs of currently-enclosing definition scopes

    # ── sub-helpers ──────────────────────────────────────────────────────────

    def record_func(c):
        usr = c.get_usr()
        if not usr or usr in func_info:
            return usr
        f = c.location.file
        func_info[usr] = {
            "name":   fully_qualified(c),
            "file":   f.name if f else "",
            "line":   c.location.line,
            "system": is_system(f.name if f else ""),
        }
        return usr

    def add_edge(caller, callee):
        if caller and callee and caller != callee:
            edges.setdefault(caller, set()).add(callee)

    def add_binding(var_usr, func_usr):
        if var_usr and func_usr:
            var_bindings.setdefault(var_usr, set()).add(func_usr)

    def add_indirect(caller, var_usr):
        if caller and var_usr:
            indirect_calls.setdefault(caller, set()).add(var_usr)

    def record_var(ref_cursor):
        """Return a var_usr for a FIELD_DECL or interesting VAR_DECL, or None."""
        kind = ref_cursor.kind
        if kind == cx.CursorKind.FIELD_DECL:
            pass  # always interesting
        elif kind == cx.CursorKind.VAR_DECL:
            parent = ref_cursor.semantic_parent
            if parent is None:
                return None
            if parent.kind in _LOCAL_SCOPE_KINDS:
                return None   # local variable — skip
            if parent.kind not in _GLOBAL_SCOPE_KINDS:
                return None   # unknown scope — skip
        else:
            return None

        usr = ref_cursor.get_usr()
        if not usr:
            return None

        if usr not in var_info:
            f      = ref_cursor.location.file
            parent = ref_cursor.semantic_parent
            var_info[usr] = {
                "name":   fully_qualified(ref_cursor),
                "type":   ref_cursor.type.spelling,
                "parent": fully_qualified(parent) if parent else "",
                "file":   f.name if f else "",
                "line":   ref_cursor.location.line,
                "system": is_system(f.name if f else ""),
            }
        return usr

    def add_var_access(func_usr, var_usr):
        if func_usr and var_usr:
            func_var_access.setdefault(func_usr, set()).add(var_usr)

    def resolve_callable(node, depth=0):
        if depth > 5:
            return []
        kind = node.kind
        if kind == cx.CursorKind.LAMBDA_EXPR:
            f = node.location.file
            return [f"lambda:{f.name if f else '?'}:{node.location.line}"]
        if kind in (cx.CursorKind.DECL_REF_EXPR, cx.CursorKind.MEMBER_REF_EXPR):
            ref = node.referenced
            if ref is not None:
                if ref.kind in FUNC_KINDS:
                    defn = ref.get_definition()
                    target = defn if defn and defn.kind in FUNC_KINDS else ref
                    u = record_func(target)
                    return [u] if u else []
                if ref.kind in (cx.CursorKind.VAR_DECL, cx.CursorKind.PARM_DECL,
                                cx.CursorKind.FIELD_DECL):
                    tc = ref.type.get_canonical().spelling
                    if type_is_std_func(ref.type.spelling) or type_is_std_func(tc):
                        u = ref.get_usr()
                        if u:
                            return [u]
        results = []
        for child in node.get_children():
            results.extend(resolve_callable(child, depth + 1))
        return results

    # ── AST walker ───────────────────────────────────────────────────────────

    def walk(node):
        entered = False

        # ── function / method / lambda scope ──────────────────────────────
        if node.kind in FUNC_KINDS and node.is_definition():
            usr = record_func(node)
            if usr:
                func_stack.append(usr)
                entered = True
                nargs = sum(1 for _ in node.get_arguments())
                sig   = (node.spelling, nargs)
                method_sigs.setdefault(sig, set()).add(usr)

        elif node.kind == cx.CursorKind.CXX_METHOD and node.is_virtual_method():
            usr = node.get_usr()
            if usr:
                record_func(node)
                nargs = sum(1 for _ in node.get_arguments())
                sig   = (node.spelling, nargs)
                method_sigs.setdefault(sig, set()).add(usr)

        elif node.kind == cx.CursorKind.LAMBDA_EXPR:
            f          = node.location.file
            lambda_usr = f"lambda:{f.name if f else '?'}:{node.location.line}"
            if lambda_usr not in func_info:
                func_info[lambda_usr] = {
                    "name":   lambda_usr,
                    "file":   f.name if f else "",
                    "line":   node.location.line,
                    "system": is_system(f.name if f else ""),
                }
            func_stack.append(lambda_usr)
            entered = True

        caller = func_stack[-1] if func_stack else None

        # ── data variable access tracking ─────────────────────────────────
        # Record every FIELD_DECL / non-local VAR_DECL that the current
        # function reads or writes.
        if caller and node.kind in (cx.CursorKind.MEMBER_REF_EXPR,
                                    cx.CursorKind.DECL_REF_EXPR):
            ref = node.referenced
            if ref is not None:
                var_usr = record_var(ref)
                if var_usr:
                    add_var_access(caller, var_usr)

        # ── call expressions ───────────────────────────────────────────────
        if node.kind == cx.CursorKind.CALL_EXPR and caller:
            ref = node.referenced

            # std::function / operator() invocation
            if (ref is not None and ref.spelling == "operator()" and
                    ref.semantic_parent is not None and
                    (type_is_std_func(ref.semantic_parent.type.spelling) or
                     type_is_std_func(
                         ref.semantic_parent.type.get_canonical().spelling))):
                recv_usr = _find_receiver_usr(node)
                if recv_usr:
                    add_indirect(caller, recv_usr)
            else:
                # direct call
                if ref is not None and ref.kind in FUNC_KINDS:
                    defn = ref.get_definition()
                    target = defn if defn and defn.kind in FUNC_KINDS else ref
                    callee = record_func(target)
                    add_edge(caller, callee)
                    try:
                        params    = list(ref.get_arguments())
                        call_args = list(node.get_arguments())
                        for param, arg in zip(params, call_args):
                            pt = param.type.spelling
                            pc = param.type.get_canonical().spelling
                            if type_is_std_func(pt) or type_is_std_func(pc):
                                p_usr = param.get_usr()
                                if p_usr:
                                    for fu in resolve_callable(arg):
                                        add_binding(p_usr, fu)
                    except Exception:
                        pass

                # OVERLOADED_DECL_REF — template method calls (e.g. Execute<T>())
                if ref is None or ref.kind not in FUNC_KINDS:
                    for child in node.get_children():
                        cref = child.referenced
                        if (child.kind == cx.CursorKind.MEMBER_REF_EXPR and
                                cref is not None and
                                cref.kind == cx.CursorKind.OVERLOADED_DECL_REF):
                            for decl in get_overloaded_decls(cref):
                                if decl.kind in FUNC_KINDS:
                                    d = decl.get_definition()
                                    target = d if d and d.kind in FUNC_KINDS else decl
                                    callee = record_func(target)
                                    if callee:
                                        add_edge(caller, callee)
                                        nargs2 = sum(1 for _ in target.get_arguments())
                                        sig2 = (target.spelling, nargs2)
                                        method_sigs.setdefault(sig2, set()).add(callee)
                            break

        # ── new-expressions: bind lambda/callable args to ctor params ──────
        if node.kind == cx.CursorKind.CXX_NEW_EXPR and caller:
            ctor_ref = node.referenced
            if ctor_ref is None or ctor_ref.kind not in FUNC_KINDS:
                ctor_ref = None
                for ch in node.get_children():
                    if ch.kind == cx.CursorKind.TYPE_REF:
                        cls_cursor = ch.referenced
                        if cls_cursor is not None:
                            cands = [m for m in cls_cursor.get_children()
                                     if m.kind == cx.CursorKind.CONSTRUCTOR]
                            if cands:
                                ctor_ref = cands[0]
                        break
            _skip = {cx.CursorKind.NAMESPACE_REF, cx.CursorKind.TYPE_REF}
            arg_exprs = [ch for ch in node.get_children()
                         if ch.kind not in _skip]
            if ctor_ref is not None:
                try:
                    params = list(ctor_ref.get_arguments())
                    for param, arg in zip(params, arg_exprs):
                        pt = param.type.spelling
                        pc = param.type.get_canonical().spelling
                        if type_is_std_func(pt) or type_is_std_func(pc):
                            p_usr = param.get_usr()
                            if p_usr:
                                for fu in resolve_callable(arg):
                                    add_binding(p_usr, fu)
                except Exception:
                    pass

        # ── std::function var/field declaration / assignment ───────────────
        if node.kind in (cx.CursorKind.VAR_DECL, cx.CursorKind.FIELD_DECL):
            tc = node.type.get_canonical().spelling
            if type_is_std_func(node.type.spelling) or type_is_std_func(tc):
                var_usr = node.get_usr()
                for child in node.get_children():
                    for fu in resolve_callable(child):
                        add_binding(var_usr, fu)

        if node.kind in (cx.CursorKind.BINARY_OPERATOR,
                          cx.CursorKind.COMPOUND_ASSIGNMENT_OPERATOR,
                          cx.CursorKind.CALL_EXPR):
            children = list(node.get_children())
            if len(children) >= 2:
                lhs, rhs = children[0], children[1]
                actual_lhs = lhs
                if (lhs.kind == cx.CursorKind.MEMBER_REF_EXPR and
                        lhs.spelling == "operator="):
                    sub = list(lhs.get_children())
                    if sub:
                        actual_lhs = sub[0]
                if actual_lhs.kind in (cx.CursorKind.MEMBER_REF_EXPR,
                                        cx.CursorKind.DECL_REF_EXPR):
                    ref = actual_lhs.referenced
                    if ref is not None:
                        tc = ref.type.get_canonical().spelling
                        if type_is_std_func(ref.type.spelling) or type_is_std_func(tc):
                            var_usr = ref.get_usr()
                            for fu in resolve_callable(rhs):
                                add_binding(var_usr, fu)

        # ── constructor member-initializer bindings ────────────────────────
        if node.kind == cx.CursorKind.CONSTRUCTOR and node.is_definition():
            ctor_children = list(node.get_children())
            i = 0
            while i < len(ctor_children) - 1:
                ch = ctor_children[i]
                if ch.kind == cx.CursorKind.MEMBER_REF:
                    ref = ch.referenced
                    if ref is not None:
                        tc = ref.type.get_canonical().spelling
                        if type_is_std_func(ref.type.spelling) or type_is_std_func(tc):
                            init_expr = ctor_children[i + 1]
                            field_usr  = ref.get_usr()
                            if field_usr:
                                for fu in resolve_callable(init_expr):
                                    add_binding(field_usr, fu)
                            i += 2
                            continue
                i += 1

        for child in node.get_children():
            walk(child)

        if entered:
            func_stack.pop()

    walk(tu.cursor)

    # Resolve indirect calls → direct edges using bindings in this TU
    for caller, var_usrs in indirect_calls.items():
        for var_usr in var_usrs:
            for fu in var_bindings.get(var_usr, ()):
                add_edge(caller, fu)

    return (func_info, edges, var_bindings, indirect_calls, method_sigs,
            func_var_access, var_info)


def _find_receiver_usr(call_expr):
    def walk(node, depth=0):
        if depth > 6:
            return None
        for child in node.get_children():
            ref = child.referenced
            if ref is not None:
                tc = ref.type.get_canonical().spelling
                if type_is_std_func(ref.type.spelling) or type_is_std_func(tc):
                    u = ref.get_usr()
                    if u:
                        return u
            r = walk(child, depth + 1)
            if r:
                return r
        return None
    return walk(call_expr)


# ── index build / load ────────────────────────────────────────────────────────

def build_index():
    db    = get_compile_db()
    files = list(db.keys())
    total = len(files)

    all_func_info       = {}
    all_edges           = {}
    all_var_bindings    = {}
    all_indirect_calls  = {}
    all_method_sigs     = {}
    all_func_var_access = {}   # func_usr -> set of var_usr
    all_var_info        = {}   # var_usr  -> info

    print(f"Indexing {total} translation units…", file=sys.stderr)
    t0 = time.time()

    for i, filepath in enumerate(files, 1):
        vlog(f"  [{i}/{total}] {os.path.relpath(filepath, WORKSPACE)}")
        tu = parse_tu(filepath)
        if tu is None:
            vlog("    ↳ parse failed, skipping")
            continue

        (func_info, edges, var_bindings, indirect_calls, method_sigs,
         func_var_access, var_info) = index_tu(filepath, tu)

        all_func_info.update(func_info)

        for caller, callees in edges.items():
            all_edges.setdefault(caller, set()).update(callees)

        for var, funcs in var_bindings.items():
            all_var_bindings.setdefault(var, set()).update(funcs)

        for caller, vars_ in indirect_calls.items():
            all_indirect_calls.setdefault(caller, set()).update(vars_)

        for sig, usrs in method_sigs.items():
            all_method_sigs.setdefault(sig, set()).update(usrs)

        for func, vars_ in func_var_access.items():
            all_func_var_access.setdefault(func, set()).update(vars_)

        all_var_info.update(var_info)

    # Transitive var_bindings expansion
    changed = True
    while changed:
        changed = False
        for var, funcs in list(all_var_bindings.items()):
            extras = set()
            for f in list(funcs):
                for ff in all_var_bindings.get(f, ()):
                    if ff not in funcs:
                        extras.add(ff)
            if extras:
                funcs.update(extras)
                changed = True

    # Resolve cross-TU indirect calls
    for caller, var_usrs in all_indirect_calls.items():
        for var_usr in var_usrs:
            for fu in all_var_bindings.get(var_usr, ()):
                all_edges.setdefault(caller, set()).add(fu)

    # Virtual dispatch expansion
    usr_to_sig = {}
    for sig, usrs in all_method_sigs.items():
        for usr in usrs:
            usr_to_sig[usr] = sig

    changed = True
    while changed:
        changed = False
        for caller, callees in list(all_edges.items()):
            extras = set()
            for callee in list(callees):
                sig = usr_to_sig.get(callee)
                if sig:
                    for co in all_method_sigs.get(sig, ()):
                        if co not in callees:
                            extras.add(co)
            if extras:
                callees.update(extras)
                changed = True

    elapsed = time.time() - t0
    n_funcs = len(all_func_info)
    n_edges = sum(len(v) for v in all_edges.values())
    n_vars  = len(all_var_info)
    print(f"Indexed {n_funcs} functions, {n_edges} edges, "
          f"{n_vars} data variables in {elapsed:.1f}s", file=sys.stderr)

    sigs_serial = {
        f"{sp}\x00{na}": list(us)
        for (sp, na), us in all_method_sigs.items()
    }
    cache = {
        "mtime":           os.path.getmtime(COMPILE_DB),
        "func_info":       all_func_info,
        "edges":           {k: list(v) for k, v in all_edges.items()},
        "var_bindings":    {k: list(v) for k, v in all_var_bindings.items()},
        "method_sigs":     sigs_serial,
        "func_var_access": {k: list(v) for k, v in all_func_var_access.items()},
        "var_info":        all_var_info,
    }
    with open(CACHE_FILE, "w") as f:
        json.dump(cache, f)
    print(f"Index written to {CACHE_FILE}", file=sys.stderr)
    return cache


def load_index():
    if os.path.exists(CACHE_FILE):
        try:
            with open(CACHE_FILE) as f:
                cache = json.load(f)
            if cache.get("mtime") == os.path.getmtime(COMPILE_DB):
                n = len(cache.get("func_info", {}))
                v = len(cache.get("var_info", {}))
                print(f"Loaded index ({n} functions, {v} variables) from cache.",
                      file=sys.stderr)
                return cache
        except Exception:
            pass
    return build_index()


# ── query (same BFS as reachability.py) ──────────────────────────────────────

def find_root_function(filepath, line):
    tu = parse_tu(filepath)
    if tu is None:
        return None, None

    src      = open(filepath).read().splitlines()
    src_line = src[line - 1] if line - 1 < len(src) else ""

    candidates = []
    m = re.search(r'(\b[A-Za-z_]\w*)\s*\(', src_line)
    if m:
        candidates.append(m.start(1) + 1)
    for m2 in re.finditer(r'\b[A-Za-z_]\w*', src_line):
        c = m2.start() + 1
        if c not in candidates:
            candidates.append(c)

    for col in candidates:
        try:
            loc = cx.SourceLocation.from_position(
                tu, tu.get_file(filepath), line, col)
            cur = cx.Cursor.from_location(tu, loc)
        except Exception:
            continue
        c = cur
        for _ in range(20):
            if c.kind in FUNC_KINDS and c.is_definition():
                if c.location.file and c.location.file.name == filepath:
                    return c.get_usr(), fully_qualified(c)
            if c.semantic_parent is None or c.semantic_parent == c:
                break
            c = c.semantic_parent

    return None, None


def query(cache, root_usr):
    edges     = cache["edges"]
    reachable = set()
    frontier  = {root_usr}
    while frontier:
        nxt = set()
        for usr in frontier:
            for callee in edges.get(usr, []):
                if callee not in reachable and callee != root_usr:
                    reachable.add(callee)
                    nxt.add(callee)
        frontier = nxt
    return reachable


# ── output ────────────────────────────────────────────────────────────────────

def _rel(path):
    try:
        return os.path.relpath(path, WORKSPACE) if path else ""
    except ValueError:
        return path


def _func_label(usr, func_info):
    info = func_info.get(usr, {})
    if not info:
        return usr, "", 0, False
    return (info.get("name", usr),
            info.get("file", ""),
            info.get("line", 0),
            info.get("system", False))


def _var_label(vinfo):
    return (f"{vinfo['name']}  [{vinfo['type']}]  "
            f"({_rel(vinfo['file'])}:{vinfo['line']})")


def print_results_by_func(root_usr, reachable, cache):
    """For each reachable function, list the data variables it touches."""
    func_info       = cache["func_info"]
    func_var_access = cache.get("func_var_access", {})
    var_info        = cache.get("var_info", {})

    all_usrs = list(reachable) + [root_usr]

    # Build sorted function list; skip intermediate var/param USRs that leaked
    # into edges during transitive expansion (same filter as reachability.py).
    funcs = []
    for usr in all_usrs:
        if usr not in func_info:
            if not usr.startswith("lambda:"):
                continue  # var/param USR — not a real function
        name, fpath, line, system = _func_label(usr, func_info)
        if not name or name == usr:
            if usr.startswith("lambda:"):
                name = usr
            else:
                continue
        if FILTER_SYSTEM and system:
            continue
        funcs.append((usr, name, fpath, line, system))

    funcs.sort(key=lambda x: (x[4], x[2], x[3]))

    printed_funcs = 0
    all_vars_seen = {}  # var_usr -> vinfo  (for summary)

    for usr, name, fpath, line, system in funcs:
        sdk = "  [sdk]" if system else ""
        print(f"\n{'─'*70}")
        print(f"{name}  ({_rel(fpath)}:{line}){sdk}")

        var_usrs = func_var_access.get(usr, [])
        # Filter system vars if requested
        visible_vars = []
        for vur in var_usrs:
            vi = var_info.get(vur)
            if vi is None:
                continue
            if FILTER_SYSTEM and vi.get("system"):
                continue
            visible_vars.append((vur, vi))
            all_vars_seen[vur] = vi

        if visible_vars:
            # Sort by parent class, then name
            visible_vars.sort(key=lambda x: (x[1].get("parent",""),
                                             x[1].get("name","")))
            for vur, vi in visible_vars:
                print(f"  data  {_var_label(vi)}")
        else:
            print("  (no tracked data variables)")

        printed_funcs += 1

    print(f"\n{'═'*70}")
    print(f"\n{printed_funcs} reachable function(s).")
    print(f"\n── Unique data variables across all reachable code "
          f"({len(all_vars_seen)}) ──")
    for vur, vi in sorted(all_vars_seen.items(),
                          key=lambda x: (x[1].get("parent",""),
                                         x[1].get("name",""))):
        print(f"  {_var_label(vi)}")


def print_results_by_var(root_usr, reachable, cache):
    """For each unique data variable, list which reachable functions touch it."""
    func_info       = cache["func_info"]
    func_var_access = cache.get("func_var_access", {})
    var_info        = cache.get("var_info", {})

    all_usrs = list(reachable) + [root_usr]

    # Build var -> [func] mapping; skip var/param USRs that leaked into edges.
    var_to_funcs = {}
    for usr in all_usrs:
        if usr not in func_info and not usr.startswith("lambda:"):
            continue
        name, fpath, line, system = _func_label(usr, func_info)
        if FILTER_SYSTEM and system:
            continue
        for vur in func_var_access.get(usr, []):
            vi = var_info.get(vur)
            if vi is None:
                continue
            if FILTER_SYSTEM and vi.get("system"):
                continue
            var_to_funcs.setdefault(vur, []).append((usr, name, fpath, line))

    if not var_to_funcs:
        print("No data variables found.")
        return

    for vur, funcs in sorted(var_to_funcs.items(),
                              key=lambda x: (var_info[x[0]].get("parent",""),
                                             var_info[x[0]].get("name",""))):
        vi = var_info[vur]
        print(f"\n{'─'*70}")
        print(f"{_var_label(vi)}")
        funcs_sorted = sorted(set(funcs), key=lambda x: (x[2], x[3]))
        for _, fname, fpath, fline in funcs_sorted:
            print(f"  func  {fname}  ({_rel(fpath)}:{fline})")

    print(f"\n{'═'*70}")
    print(f"\n{len(var_to_funcs)} unique data variable(s) accessed.")


# ── main ──────────────────────────────────────────────────────────────────────

def main():
    args = sys.argv[1:]

    if not args or args[0] == "--help":
        print(__doc__, file=sys.stderr)
        sys.exit(0)

    if args[0] == "--index":
        build_index()
        return

    if len(args) < 2 or not args[1].isdigit():
        print(f"Usage: {sys.argv[0]} <file> <line>", file=sys.stderr)
        sys.exit(1)

    filepath    = os.path.abspath(args[0])
    line_1based = int(args[1])

    if not os.path.isfile(filepath):
        print(f"Error: not found: {filepath}", file=sys.stderr)
        sys.exit(1)

    cache = load_index()

    print(f"Locating function at "
          f"{os.path.relpath(filepath, WORKSPACE)}:{line_1based}…",
          file=sys.stderr)
    root_usr, root_name = find_root_function(filepath, line_1based)
    if root_usr is None:
        print("No function definition found at that line.", file=sys.stderr)
        sys.exit(1)
    print(f"Root: {root_name}  (USR: {root_usr[:60]}…)", file=sys.stderr)

    reachable = query(cache, root_usr)
    print(f"Found {len(reachable)} reachable functions.\n", file=sys.stderr)

    if GROUP == "var":
        print_results_by_var(root_usr, reachable, cache)
    else:
        print_results_by_func(root_usr, reachable, cache)


if __name__ == "__main__":
    main()
