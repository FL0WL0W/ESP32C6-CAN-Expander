#!/home/daniel/.espressif/tools/python/v5.5.2/venv/bin/python
"""
iram_fragment.py — generate ESP-IDF linker fragments for IRAM/DRAM placement

Reads the data_usage index to find all reachable functions and global/static
data variables from one or more entry-point functions, then emits a .lf
linker fragment file that places all of them in IRAM (code) / DRAM (data),
including vtables (which otherwise default to flash-mapped .rodata).

Usage:
    python iram_fragment.py <file>:<line> [<file>:<line> ...]
    python iram_fragment.py Esp32::RAMTimerInterrupt [OtherNs::Func ...]
    python iram_fragment.py @entrypoints.txt        # one entry per line
    python iram_fragment.py -o iram_critical.lf <file>:<line> ...

Entry points can be:
    file:line         Absolute or relative path with 1-based line number
    C++::Name         Unmangled C++ function name (namespace::Function);
                      all overloads / constructors with that base name are used.

Options:
    -o <outfile>      Write fragment to <outfile> (default: iram_critical.lf)
    --name <name>     Mapping name in the fragment (default: iram_critical)
    --vtables-only    Only emit vtable entries (skip functions/data)

Env vars:
    FILTER_SYSTEM=1   Skip functions in .espressif/ (default 1)
    VERBOSE=1         Print per-symbol resolution details
"""

import sys, os, re, json, subprocess, shlex, time
sys.path.insert(0, "/home/daniel/.espressif/tools/python/v5.5.2/venv/lib/"
                   "python3.12/site-packages")
import clang.cindex as cx

# ── constants ─────────────────────────────────────────────────────────────────

LIBCLANG   = ("/home/daniel/.espressif/tools/esp-clang-libs/"
              "esp-19.1.2_20250312/esp-clang/lib/libclang.so")
WORKSPACE  = os.path.dirname(os.path.abspath(__file__))
COMPILE_DB = os.path.join(WORKSPACE, "build", "compile_commands.json")
BUILD_DIR  = os.path.join(WORKSPACE, "build")
DATA_CACHE = os.path.join(WORKSPACE, ".data_usage_index.json")

FILTER_SYSTEM = os.environ.get("FILTER_SYSTEM", "0") == "0"
VERBOSE       = os.environ.get("VERBOSE",       "0") == "1"

SYSTEM_PREFIXES = ("/home/daniel/.espressif/", "/usr/", "/opt/")

FUNC_KINDS = {
    cx.CursorKind.FUNCTION_DECL,
    cx.CursorKind.FUNCTION_TEMPLATE,
    cx.CursorKind.CXX_METHOD,
    cx.CursorKind.CONSTRUCTOR,
    cx.CursorKind.DESTRUCTOR,
    cx.CursorKind.CONVERSION_FUNCTION,
}

NM = "nm"  # use system nm; riscv cross-nm is not needed for symbol table reading

# ── helpers ───────────────────────────────────────────────────────────────────

def vlog(*args):
    if VERBOSE:
        print(*args, file=sys.stderr)


def strip_params(s):
    """Strip function parameters AND all template arguments (iteratively).

    'std::list<Task*, allocator<Task*>>::pop_front(Task*)'
    → 'std::list::pop_front'

    This normalises both the sym_index keys (built from fully-instantiated
    demangled names) and the lookup targets (func_info names which carry no
    template specialisation) so they meet on the same canonical string.
    """
    # 1. Iteratively strip innermost <...> FIRST — before touching parens.
    #    This prevents the paren-strip from firing inside template args like
    #    <void ()>, which would truncate 'std::function<void ()>::operator()'
    #    to just 'std::function<void' instead of 'std::function::operator'.
    while True:
        s2 = re.sub(r'<[^<>]*>', '', s)
        if s2 == s:
            break
        s = s2
    # 2. Strip [abi:...] suffixes (e.g. remove[abi:__cxx20])
    s = re.sub(r'\[abi:[^\]]*\]', '', s)
    # 3. Now strip function parameters (safe: no template brackets remain)
    s = re.sub(r'\(.*', '', s)
    return s.strip()


def is_system(path):
    return not path or any(path.startswith(p) for p in SYSTEM_PREFIXES)


def rel(path):
    try:
        return os.path.relpath(path, WORKSPACE) if path else ""
    except ValueError:
        return path


# ── load index ────────────────────────────────────────────────────────────────

def load_data_index():
    if not os.path.exists(DATA_CACHE):
        print("ERROR: data_usage index not found.  "
              "Run data_usage.py --index first.", file=sys.stderr)
        sys.exit(1)
    with open(DATA_CACHE) as f:
        cache = json.load(f)
    n_f = len(cache.get("func_info", {}))
    n_v = len(cache.get("var_info", {}))
    print(f"Loaded data_usage index ({n_f} functions, {n_v} variables).",
          file=sys.stderr)
    return cache


# ── root function location ────────────────────────────────────────────────────

def find_root_usr(cache, filepath, line):
    """Find the USR of the function at filepath:line using libclang."""
    cx.Config.set_library_file(LIBCLANG)
    idx   = cx.Index.create()

    with open(COMPILE_DB) as f:
        db_list = json.load(f)
    db = {e["file"]: e for e in db_list}

    entry = db.get(filepath)
    if not entry:
        base = os.path.basename(filepath)
        for k, v in db.items():
            if os.path.basename(k) == base:
                entry = v; break
    if not entry:
        print(f"WARNING: {filepath} not found in compile_commands.json",
              file=sys.stderr)
        args = []
    else:
        raw  = shlex.split(entry.get("command", ""))
        args = [a for a in raw[1:]
                if not a.endswith((".c", ".cpp", ".cxx", ".cc"))
                and a not in ("-c", "-o")
                and not a.startswith(("-M", "-o"))]

    try:
        tu = idx.parse(
            filepath, args=args,
            options=(cx.TranslationUnit.PARSE_DETAILED_PROCESSING_RECORD |
                     cx.TranslationUnit.PARSE_INCOMPLETE),
        )
    except Exception as e:
        print(f"Parse error for {filepath}: {e}", file=sys.stderr)
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
                    return c.get_usr(), c.spelling
            if c.semantic_parent is None or c.semantic_parent == c:
                break
            c = c.semantic_parent

    return None, None


# ── BFS reachability from multiple roots ──────────────────────────────────────

def query_multi(cache, root_usrs):
    edges     = cache["edges"]
    reachable = set(root_usrs)
    frontier  = set(root_usrs)
    while frontier:
        nxt = set()
        for usr in frontier:
            for callee in edges.get(usr, []):
                if callee not in reachable:
                    reachable.add(callee)
                    nxt.add(callee)
        frontier = nxt
    return reachable


# ── build comprehensive symbol index from all archives ────────────────────────

def build_symbol_index():
    """
    nm every non-bootloader archive in build/.
    Returns:
      sym_index : demangled_name -> list of (arc_base, obj_base, mangled, type_char)
      vtab_index: arc_base -> obj_base -> [mangled_vtable_sym, ...]
      arc_map   : arc_base -> arc_abs_path
    """
    print("Scanning archives for symbol index…", file=sys.stderr)

    archives = []
    for root, dirs, files in os.walk(BUILD_DIR):
        dirs[:] = [d for d in dirs
                   if d not in ("bootloader", "bootloader-prefix")
                   and not d.endswith("-subbuild")]
        for fname in files:
            if fname.endswith(".a"):
                archives.append(os.path.join(root, fname))

    sym_index      = {}   # demangled -> [(arc_base, obj_base, mangled, type_char)]
    vtab_index     = {}   # arc_base  -> {obj_base -> [mangled, ...]}
    arc_map        = {}   # arc_base  -> arc_abs_path
    mang_to_demang = {}   # mangled  -> demangled
    invoke_index   = {}   # obj_base  -> [(arc_base, mangled)] for _M_invoke/_M_manager lambdas

    for arc_abs in sorted(archives):
        arc_base = os.path.basename(arc_abs)
        arc_map.setdefault(arc_base, arc_abs)

        # nm the whole archive — gets all members at once
        try:
            r_m = subprocess.run(
                [NM, "--defined-only", arc_abs],
                capture_output=True, text=True, timeout=60)
            r_d = subprocess.run(
                [NM, "--defined-only", "-C", arc_abs],
                capture_output=True, text=True, timeout=60)
        except Exception:
            continue

        lines_m = r_m.stdout.splitlines()
        lines_d = r_d.stdout.splitlines()
        if len(lines_m) != len(lines_d):
            # Fallback: zip as far as possible
            pass

        cur_obj = None
        d_iter  = iter(lines_d)

        for line_m in lines_m:
            line_d = next(d_iter, "")

            # Archive member header looks like "TimerService.cpp.obj:"
            if line_m.endswith(":") and not line_m.startswith(" "):
                cur_obj = line_m.rstrip(":")
                continue
            if not cur_obj:
                continue

            parts_m = line_m.split()
            parts_d = line_d.split()
            if len(parts_m) < 3 or len(parts_d) < 3:
                continue

            type_char = parts_m[-2]
            mangled   = parts_m[-1]
            demangled = " ".join(parts_d[2:])

            # Record demangled name for every symbol
            mang_to_demang[mangled] = demangled

            # vtables and typeinfo
            if mangled.startswith(("_ZTV", "_ZTI")):
                vtab_index.setdefault(arc_base, {}).setdefault(
                    cur_obj, []).append(mangled)
                continue

            # Lambda invoke/manager wrappers — index by obj_base.
            # Include weak (W/w) because header-file template lambdas are weak.
            if (type_char in "TtWw" and
                    ("_M_invoke" in mangled or "_M_manager" in mangled)):
                invoke_index.setdefault(cur_obj, []).append(
                    (arc_base, mangled))
                # Also fall through to sym_index so _M_invoke can be found
                # by demangled lookup if ever needed.

            # Index code and data symbols
            if type_char in "TtWwDdBbRrGgSs":
                key = strip_params(demangled)
                sym_index.setdefault(key, []).append(
                    (arc_base, cur_obj, mangled, type_char))

    n_invoke = sum(len(v) for v in invoke_index.values())
    print(f"  {len(sym_index)} unique demangled symbols, "
          f"{n_invoke} lambda invoke/manager symbols across "
          f"{len(arc_map)} archives.", file=sys.stderr)
    return sym_index, vtab_index, arc_map, mang_to_demang, invoke_index


def lookup_function(sym_index, func_name):
    """
    Find all (arc_base, obj_base, mangled) entries for a fully-qualified
    function name.  Prefers exact FQ match, falls back to short-name match.
    Returns list (may be multiple if weak symbol in several objects).
    """
    target = strip_params(func_name)
    results = sym_index.get(target, [])
    if results:
        return [(a, o, m) for a, o, m, t in results if t in "TtWw"]

    # Try short name fallback — last component
    short = target.split("::")[-1]
    candidates = []
    for key, entries in sym_index.items():
        if key == short or key.endswith("::" + short):
            if target in key or short == target:
                for a, o, m, t in entries:
                    if t in "TtWw" and (a, o, m) not in candidates:
                        candidates.append((a, o, m))
    return candidates


def lookup_data(sym_index, var_name):
    """Find all (arc_base, obj_base, mangled) for a global/static variable."""
    target = strip_params(var_name)
    results = sym_index.get(target, [])
    if results:
        return [(a, o, m) for a, o, m, t in results if t in "DdBbRrGgSs"]

    short = target.split("::")[-1]
    candidates = []
    for key, entries in sym_index.items():
        if key == short or key.endswith("::" + short):
            for a, o, m, t in entries:
                if t in "DdBbRrGgSs" and (a, o, m) not in candidates:
                    candidates.append((a, o, m))
    return candidates


# ── main generation ───────────────────────────────────────────────────────────

def _obj_stem(obj_base):
    """'Esp32IdfTimerService.cpp.obj' -> 'Esp32IdfTimerService'"""
    stem = obj_base
    if stem.endswith('.obj'):
        stem = stem[:-4]                    # strip .obj
        dot = stem.rfind('.')               # strip .cpp / .c / etc.
        if dot != -1:
            stem = stem[:dot]
    return stem


def _resolve_lambda_syms(func_name, invoke_index, mang_to_demang):
    """
    For a lambda USR like 'lambda:/path/to/Foo.cpp:16',
    find all _M_invoke/_M_manager symbols from the obj compiled from Foo.cpp.

    For .cpp/.c source files: obj_base stem must match (fast, exact).
    For .h/.hpp header files: fall back to searching all invoke_index entries
    whose demangled name contains the header's class stem (weak symbols from
    template instantiations can live in any including obj).

    Returns list of (arc_base, obj_base, mangled).
    """
    if not func_name.startswith("lambda:"):
        return []
    rest = func_name[len("lambda:"):]      # "/path/to/Foo.cpp:16"
    colon = rest.rfind(":")
    if colon == -1:
        return []
    src_path = rest[:colon]                # "/path/to/Foo.cpp"
    src_base = os.path.basename(src_path)  # "Foo.cpp" or "Foo.h"
    src_stem = os.path.splitext(src_base)[0]  # "Foo"

    # Is this a header-file lambda?
    src_ext = os.path.splitext(src_base)[1].lower()
    is_header = src_ext in (".h", ".hpp", ".hxx", ".hh", "")

    results = []
    seen = set()

    # Primary: match obj stem to source file stem (works for .cpp lambdas)
    for obj_base, entries in invoke_index.items():
        if _obj_stem(obj_base) == src_stem:
            for arc_base, mangled in entries:
                key = (arc_base, obj_base, mangled)
                if key not in seen:
                    seen.add(key)
                    results.append(key)

    # Fallback for header-file lambdas: scan all invoke symbols whose
    # demangled name mentions the stem class (picks up weak template instances)
    if not results and is_header:
        for obj_base, entries in invoke_index.items():
            for arc_base, mangled in entries:
                dem = mang_to_demang.get(mangled, "")
                if src_stem in dem:
                    key = (arc_base, obj_base, mangled)
                    if key not in seen:
                        seen.add(key)
                        results.append(key)

    return results


def generate_fragment(roots, cache, sym_index, vtab_index, arc_map,
                      mang_to_demang, invoke_index, mapping_name, outfile):
    """
    BFS, then for each reachable function/variable find its archive placement
    and emit linker fragment entries.
    """
    func_info       = cache.get("func_info", {})
    var_info        = cache.get("var_info", {})
    func_var_access = cache.get("func_var_access", {})

    # BFS from all roots
    reachable_usrs = query_multi(cache, roots)
    print(f"Total reachable functions: {len(reachable_usrs)}", file=sys.stderr)

    # ── 1. Collect reachable global/static vars ───────────────────────────
    all_var_usrs = set()
    for usr in reachable_usrs:
        for vur in func_var_access.get(usr, []):
            vi = var_info.get(vur, {})
            if FILTER_SYSTEM and vi.get("system"):
                continue
            all_var_usrs.add(vur)

    # ── 2. Collect vtable sets for archives that have reachable code ──────
    # arc_base -> {obj_base -> {code: set, data: set, vtable: set}}
    arc_entries = {}

    def get_entry(arc_base, obj_base):
        arc_entries.setdefault(arc_base, {})
        arc_entries[arc_base].setdefault(
            obj_base, {"code": set(), "data": set(), "vtable": set()})
        return arc_entries[arc_base][obj_base]

    unresolved_funcs = []
    unresolved_vars  = []

    # Functions → code entries
    for usr in reachable_usrs:
        info = func_info.get(usr)
        if info is None:
            continue
        is_sys = FILTER_SYSTEM and info.get("system")

        results = lookup_function(sym_index, info["name"])
        if not results and info["name"].startswith("lambda:"):
            results = _resolve_lambda_syms(info["name"], invoke_index,
                                           mang_to_demang)
            if results:
                vlog(f"  lambda→invoke {info['name']} → {len(results)} syms")
        if not results:
            if not is_sys:
                # Only report truly-user functions as unresolved
                vlog(f"  UNRESOLVED func: {info['name']}")
                unresolved_funcs.append(info["name"])
            else:
                # System symbol not found in user archives — true ROM/libgcc,
                # silently drop (it lives in ROM or a system lib we don't place)
                vlog(f"  SKIP system func: {info['name']}")
            continue

        for arc_base, obj_base, mangled in results:
            vlog(f"  func  {arc_base}/{obj_base}: {mangled}")
            e = get_entry(arc_base, obj_base)
            e["code"].add(mangled)
            # Collect vtables from this object file
            for vm in vtab_index.get(arc_base, {}).get(obj_base, []):
                e["vtable"].add(vm)

    # ── 2b. Weak-symbol saturation pass ──────────────────────────────────
    # For every code symbol already placed, find ALL other obj files across
    # all archives that define the same function (same normalised demangled
    # name) as a weak (W/w) symbol and emit noflash for those copies too.
    # This is necessary because the linker resolves weak symbols to the FIRST
    # defining obj it encounters, which may not be the one we tagged.
    # We build a map: mangled → set-of-arc_entries keys already placed, then
    # extend to every sym_index entry with the same normalised name.
    placed_names = set()
    for obj_map in arc_entries.values():
        for e in obj_map.values():
            for mangled in e["code"]:
                placed_names.add(strip_params(mang_to_demang.get(mangled, mangled)))

    for norm_name in placed_names:
        for arc_base, obj_base, mangled, type_char in sym_index.get(norm_name, []):
            if type_char not in "Ww":
                continue  # strong symbols are already covered by reachability
            e = get_entry(arc_base, obj_base)
            if mangled not in e["code"]:
                vlog(f"  weak-cover {arc_base}/{obj_base}: {mangled}")
                e["code"].add(mangled)

    # Global/static variables → data entries
    for vur in all_var_usrs:
        vi = var_info.get(vur, {})
        if not vi:
            continue
        is_sys_var = FILTER_SYSTEM and vi.get("system")

        results = lookup_data(sym_index, vi["name"])
        if not results:
            if not is_sys_var:
                vlog(f"  UNRESOLVED data: {vi['name']}")
                unresolved_vars.append(vi["name"])
            else:
                vlog(f"  SKIP system data: {vi['name']}")
            continue

        for arc_base, obj_base, mangled in results:
            vlog(f"  data  {arc_base}/{obj_base}: {mangled}")
            e = get_entry(arc_base, obj_base)
            e["data"].add(mangled)

    # ── 3. Emit linker fragment ────────────────────────────────────────────
    # ESP-IDF's grammar allows exactly ONE archive: per [mapping:] block,
    # so we emit one [mapping:] block per archive.

    def arc_tag(arc_base):
        """'libEFIGenie.a' -> 'EFIGenie', 'libmain.a' -> 'main'"""
        name = arc_base
        if name.endswith(".a"):
            name = name[:-2]
        if name.startswith("lib"):
            name = name[3:]
        # Make safe identifier (replace non-alphanums with _)
        name = re.sub(r'[^A-Za-z0-9_]', '_', name)
        return name

    lines = [
        f"# Auto-generated by iram_fragment.py",
        f"# Entry points: {', '.join(ROOT_LABELS)}",
        f"# Generated: {time.strftime('%Y-%m-%d %H:%M:%S')}",
        f"#",
        f"# noflash       -> place function in IRAM (.iram0.text)",
        f"# noflash_data  -> place data/vtable in DRAM (.dram0.data / .dram0.rodata)",
        f"# One [mapping:] block per archive (ldgen grammar restriction).",
        f"",
    ]

    for arc_base in sorted(arc_entries.keys()):
        obj_map = arc_entries[arc_base]

        # Collect non-empty entries for this archive
        arc_lines = []
        for obj_base in sorted(obj_map.keys()):
            e     = obj_map[obj_base]
            code  = sorted(e["code"])
            data  = sorted(e["data"])
            vtabs = sorted(e["vtable"])

            if not code and not data and not vtabs:
                continue

            stem = _obj_stem(obj_base)
            arc_lines.append(f"    # {obj_base}")
            for sym in code:
                dem = mang_to_demang.get(sym, sym)
                arc_lines.append(f"    {stem}:{sym} (noflash)  "
                                 f"# {dem}")
            for sym in data:
                dem = mang_to_demang.get(sym, sym)
                arc_lines.append(f"    {stem}:{sym} (noflash_data)  "
                                 f"# {dem}")
            for sym in vtabs:
                dem = mang_to_demang.get(sym, sym)
                arc_lines.append(f"    {stem}:{sym} (noflash_data)  "
                                 f"# {dem}")
            arc_lines.append("")

        if not arc_lines:
            continue

        tag = f"{mapping_name}_{arc_tag(arc_base)}"
        lines.append(f"[mapping:{tag}]")
        lines.append(f"archive: {arc_base}")
        lines.append(f"entries:")
        lines.extend(arc_lines)
        lines.append("")

    # ── 4. Unresolved summary (comment block) ────────────────────────────
    if unresolved_funcs or unresolved_vars:
        lines.append("# -- Unresolved (not placed -- may need IRAM_ATTR in source) --")
        for name in sorted(set(unresolved_funcs)):
            lines.append(f"#   func  {name}")
        for name in sorted(set(unresolved_vars)):
            lines.append(f"#   data  {name}")
        lines.append("")

    fragment_text = "\n".join(lines)

    if outfile == "-":
        print(fragment_text)
    else:
        with open(outfile, "w") as f:
            f.write(fragment_text)
        print(f"\nLinker fragment written to: {outfile}", file=sys.stderr)

    # ── 5. Stats ──────────────────────────────────────────────────────────
    total_code      = sum(len(e["code"])   for od in arc_entries.values()
                          for e in od.values())
    total_data      = sum(len(e["data"])   for od in arc_entries.values()
                          for e in od.values())
    total_vtabs     = sum(len(e["vtable"]) for od in arc_entries.values()
                          for e in od.values())
    print(f"\nPlaced: {total_code} functions, {total_data} data symbols, "
          f"{total_vtabs} vtable/typeinfo symbols.", file=sys.stderr)
    print(f"Unresolved: {len(set(unresolved_funcs))} functions, "
          f"{len(set(unresolved_vars))} data "
          f"(likely instance fields — not independently placeable).",
          file=sys.stderr)



# ── argument parsing ──────────────────────────────────────────────────────────

ROOT_LABELS = []   # filled in main for use in emit

def parse_args(argv):
    outfile      = "iram_critical.lf"
    mapping_name = "iram_critical"
    entry_strs   = []

    i = 0
    while i < len(argv):
        arg = argv[i]
        if arg in ("-h", "--help"):
            print(__doc__, file=sys.stderr); sys.exit(0)
        elif arg == "-o":
            i += 1; outfile = argv[i]
        elif arg == "--name":
            i += 1; mapping_name = argv[i]
        elif arg.startswith("@"):
            listfile = arg[1:]
            with open(listfile) as f:
                for line in f:
                    line = line.strip()
                    if line and not line.startswith("#"):
                        entry_strs.append(line)
        else:
            entry_strs.append(arg)
        i += 1

    return outfile, mapping_name, entry_strs


def _is_file_line(s):
    """Return True if s looks like a file:line entry (last colon followed by digits)."""
    colon = s.rfind(":")
    if colon == -1:
        return False
    return s[colon + 1:].isdigit()


def resolve_entries(entry_strs):
    """
    Split entry strings into two lists:
      file_entries   : list of (abs_path, line_int)
      symbol_entries : list of unmangled C++ name strings
    """
    file_entries   = []
    symbol_entries = []
    for s in entry_strs:
        if _is_file_line(s):
            parts = s.rsplit(":", 1)
            fpath = os.path.abspath(parts[0])
            if not os.path.isfile(fpath):
                print(f"File not found: {fpath}", file=sys.stderr)
                continue
            file_entries.append((fpath, int(parts[1])))
        else:
            # Treat as a C++ unmangled symbol name
            symbol_entries.append(s)
    return file_entries, symbol_entries


def find_root_usrs_by_name(cache, symbol_name):
    """
    Find all USRs in func_info whose normalized demangled name matches
    symbol_name (exact or as a suffix after '::'). Returns (usrs, matched_names).
    """
    func_info = cache.get("func_info", {})
    target = strip_params(symbol_name)

    exact   = []  # strip_params(name) == target
    suffix  = []  # strip_params(name) ends with '::' + target

    for usr, info in func_info.items():
        norm = strip_params(info.get("name", ""))
        if norm == target:
            exact.append((usr, info["name"]))
        elif norm.endswith("::" + target):
            suffix.append((usr, info["name"]))

    results = exact if exact else suffix
    usrs   = [u for u, _ in results]
    names  = [n for _, n in results]
    return usrs, names


# ── main ──────────────────────────────────────────────────────────────────────

def main():
    global ROOT_LABELS
    argv = sys.argv[1:]
    if not argv:
        print(__doc__, file=sys.stderr); sys.exit(0)

    outfile, mapping_name, entry_strs = parse_args(argv)
    file_entries, symbol_entries = resolve_entries(entry_strs)

    if not file_entries and not symbol_entries:
        print("No valid entry points provided.", file=sys.stderr)
        sys.exit(1)

    cache = load_data_index()

    root_usrs  = []
    root_names = []

    # ── File:line entry points (resolve via libclang) ────────────────────────
    for fpath, line in file_entries:
        label = f"{rel(fpath)}:{line}"
        print(f"Locating entry point {label}…", file=sys.stderr)
        usr, name = find_root_usr(cache, fpath, line)
        if usr is None:
            print(f"  WARNING: no function found at {label}", file=sys.stderr)
            continue
        print(f"  → {name}", file=sys.stderr)
        root_usrs.append(usr)
        root_names.append(name)
        ROOT_LABELS.append(label)

    # ── Symbol-name entry points (resolve via func_info cache) ───────────────
    for sym in symbol_entries:
        print(f"Locating entry point '{sym}'…", file=sys.stderr)
        usrs, names = find_root_usrs_by_name(cache, sym)
        if not usrs:
            print(f"  WARNING: '{sym}' not found in data_usage index",
                  file=sys.stderr)
            continue
        for usr, name in zip(usrs, names):
            print(f"  → {name}", file=sys.stderr)
            root_usrs.append(usr)
            root_names.append(name)
        ROOT_LABELS.append(sym)

    if not root_usrs:
        print("No entry points resolved.", file=sys.stderr)
        sys.exit(1)

    print("Building symbol index from archives…", file=sys.stderr)
    sym_index, vtab_index, arc_map, mang_to_demang, invoke_index = \
        build_symbol_index()

    generate_fragment(root_usrs, cache, sym_index, vtab_index, arc_map,
                      mang_to_demang, invoke_index, mapping_name, outfile)


if __name__ == "__main__":
    main()
