#!/usr/bin/env python3
"""
GIMPLE Call Path Tracer
Parses GIMPLE output files and traces function call paths for a specified function.
Supports analyzing multiple GIMPLE files to build a complete call graph.
"""

import re
import sys
from pathlib import Path
from dataclasses import dataclass, field
from typing import Dict, List, Set, Optional, Tuple
import glob
import subprocess


@dataclass
class FunctionCall:
    """Represents a function call found in GIMPLE"""
    name: str
    line_number: int
    source_file: Optional[str] = None
    source_line: Optional[int] = None
    gimple_file: Optional[str] = None


@dataclass
class Function:
    """Represents a function definition in GIMPLE"""
    name: str
    start_line: int
    end_line: int
    calls: List[FunctionCall] = field(default_factory=list)
    source_file: Optional[str] = None
    source_line: Optional[int] = None
    gimple_file: Optional[str] = None


class GimpleParser:
    """Parses GIMPLE intermediate representation files"""

    def __init__(self, gimple_files: List[str]):
        self.gimple_files = [Path(f) for f in gimple_files]
        self.functions: Dict[str, Function] = {}
        self.all_calls: List[Tuple[str, FunctionCall]] = []  # (caller, call)
        # vtables: demangled class name -> {slot_index -> demangled_func_name}
        self.vtables: Dict[str, Dict[int, str]] = {}
        # vptr_map: interface_name -> [(vtable_key, vptr_byte_offset)]
        self.vptr_map: Dict[str, List[Tuple[str, int]]] = {}

        # Phase 1 data (fast scan of every file)
        self._vtable_obj_candidates: List[str] = []
        self._raw_vptr_pairs: List[Tuple[str, str, int]] = []  # (iface, mangled_ztv, offset)
        self._vptr_mangled_names: Set[str] = set()

        # Lazy-loading state
        # Maps function name -> the gimple file it lives in
        self._func_to_file: Dict[str, str] = {}
        # Set of gimple file paths whose call bodies have already been parsed
        self._parsed_files: Set[str] = set()

        self._fast_scan_all()   # read every file: collect func names + ZTV data
        self._scan_vtables()    # readelf on flagged .obj files
        self._build_vptr_map()  # demangle + correlate (vtable data now complete)

    def _scan_vtables(self):
        """Scan the .obj files flagged during GIMPLE parsing for vtable definitions."""
        self.vtables = scan_obj_vtables(self._vtable_obj_candidates)

    def _build_vptr_map(self):
        """
        Build the interface -> [(vtable_key, vptr_byte_offset)] map from the
        vptr assignment pairs collected during _parse_file.
        """
        if not self._raw_vptr_pairs:
            return

        mangled_list = sorted(self._vptr_mangled_names)
        try:
            result = subprocess.run(
                ['c++filt'],
                input='\n'.join(mangled_list),
                capture_output=True, text=True
            )
            demangled_lines = result.stdout.strip().splitlines()
        except Exception:
            demangled_lines = mangled_list

        demangle_map = dict(zip(mangled_list, demangled_lines))
        vtable_key_map = {
            m: d.replace('vtable for ', '')
            for m, d in demangle_map.items()
        }

        for iface, mangled_ztv, offset in self._raw_vptr_pairs:
            vtable_key = vtable_key_map.get(mangled_ztv, mangled_ztv)
            if vtable_key not in self.vtables:
                continue
            entry = (vtable_key, offset)
            if iface not in self.vptr_map:
                self.vptr_map[iface] = []
            if entry not in self.vptr_map[iface]:
                self.vptr_map[iface].append(entry)

    def _ensure_file_parsed(self, func_name: str):
        """Ensure the GIMPLE file containing func_name has been fully parsed for calls."""
        file_path = self._func_to_file.get(func_name)
        if file_path and file_path not in self._parsed_files:
            self._parsed_files.add(file_path)  # mark before parsing to prevent re-entry
            self._parse_file_full(Path(file_path))

    def _ensure_all_parsed(self):
        """Force full parsing of every GIMPLE file (used by --list / --stats)."""
        for gf in self.gimple_files:
            fp = str(gf)
            if fp not in self._parsed_files:
                self._parsed_files.add(fp)
                self._parse_file_full(gf)

    def _resolve_virtual(self, iface: str, token: int) -> List[str]:
        """
        Resolve an OBJ_TYPE_REF virtual call to concrete implementations.

        token is the slot index relative to the vptr (the decimal N from NB in GIMPLE).
        vptr_byte_offset is the byte offset from the ZTV symbol where the vptr points;
        the actual vtable slot = token + (vptr_byte_offset - 8) / 4.
        """
        results = []
        for vtable_key, vptr_byte_offset in self.vptr_map.get(iface, []):
            slot = token + (vptr_byte_offset - 8) // 4
            slots = self.vtables.get(vtable_key, {})
            func = slots.get(slot)
            if func and func != '__cxa_pure_virtual':
                results.append(func)
        return results

    def _fast_scan_all(self):
        """Phase 1: read every GIMPLE file once to collect function names and ZTV data."""
        for gf in self.gimple_files:
            self._fast_scan_file(gf)


    def _fast_scan_file(self, gimple_file: Path):
        """Lightweight scan of one GIMPLE file.

        Collects:
          - Function names (+ source location) -> skeleton Function objects
          - _func_to_file mapping (name -> file path)
          - ZTV/vptr assignment pairs for vtable resolution
          - Flags companion .obj file for vtable scanning

        Does NOT parse function call bodies — that is deferred to _parse_file_full.
        """
        try:
            with open(gimple_file, 'r', encoding='utf-8', errors='ignore') as f:
                lines = f.readlines()
        except Exception as e:
            print(f"Warning: Could not read {gimple_file}: {e}", file=sys.stderr)
            return

        ztv_assign_re = re.compile(r'(_\d+)\s*=\s*&\s*(_ZTV[A-Za-z0-9_]+)\s*\+\s*(\d+)')
        vptr_write_re = re.compile(r'_vptr\.([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(_\d+)')
        temp_vars: Dict[str, Tuple[str, int]] = {}
        has_ztv = False
        has_cxx = False

        i = 0
        while i < len(lines):
            line = lines[i]
            stripped = line.strip()

            # --- ZTV / vptr pair collection (needed for vtable resolution) ---
            stripped_for_vptr = stripped
            bracket = line.find('] ')
            if bracket >= 0:
                stripped_for_vptr = line[bracket + 2:].strip()
            m = ztv_assign_re.search(stripped_for_vptr)
            if m:
                has_ztv = True
                temp_vars[m.group(1)] = (m.group(2), int(m.group(3)))
                self._vptr_mangled_names.add(m.group(2))
            else:
                m = vptr_write_re.search(stripped_for_vptr)
                if m and m.group(2) in temp_vars:
                    mangled_ztv, offset = temp_vars[m.group(2)]
                    self._raw_vptr_pairs.append((m.group(1), mangled_ztv, offset))

            # --- Function definition detection ---
            is_cxx_candidate = '::' in stripped and ')' in stripped
            is_c_candidate = (
                '::' not in stripped
                and '(' in stripped
                and stripped.endswith(')')
                and not line[0:1].isspace()
                and not stripped.startswith('[')
                and not stripped.startswith('<')
            )

            if is_cxx_candidate or is_c_candidate:
                has_brace = '{' in stripped
                brace_line = line
                if not has_brace and i + 1 < len(lines):
                    next_line = lines[i + 1]
                    if '{' in next_line:
                        brace_line = next_line
                        has_brace = True

                if has_brace:
                    func_match = re.search(
                        r'((?:[a-zA-Z_][a-zA-Z0-9_]*(?:<[^>]*>)?::)*~?[a-zA-Z_][a-zA-Z0-9_]*(?:<[^>]*>)?)\s*\(',
                        stripped
                    )
                    if func_match:
                        func_name = func_match.group(1).strip()
                        source_info = self._extract_source_info(brace_line)
                        if source_info[0] is None:
                            source_info = self._extract_source_info(line)

                        func = Function(
                            name=func_name,
                            start_line=i,
                            end_line=0,
                            source_file=source_info[0],
                            source_line=source_info[1],
                            gimple_file=str(gimple_file)
                        )
                        if func_name not in self.functions:
                            self.functions[func_name] = func
                            self._func_to_file[func_name] = str(gimple_file)
                        elif source_info[0] is not None:
                            self.functions[func_name] = func
                            self._func_to_file[func_name] = str(gimple_file)

                        if '::' in func_name:
                            has_cxx = True
            i += 1

        # Flag companion .obj for vtable scanning.
        if has_ztv or has_cxx:
            base = str(gimple_file)[:-len('.006t.gimple')]
            dot = base.rfind('.')
            if dot >= 0:
                base = base[:dot]
            obj = base + '.obj'
            if Path(obj).exists() and obj not in self._vtable_obj_candidates:
                self._vtable_obj_candidates.append(obj)

    def _parse_file_full(self, gimple_file: Path):
        """Full parse of one GIMPLE file: populate call bodies for every function in it.

        This is called on-demand (via _ensure_file_parsed) only for files that
        contain functions we actually visit during call-tree traversal.
        Vtable data is already complete when this runs, so virtual calls are
        resolved immediately — no placeholder mechanism needed.
        """
        try:
            with open(gimple_file, 'r', encoding='utf-8', errors='ignore') as f:
                lines = f.readlines()
        except Exception as e:
            print(f"Warning: Could not read {gimple_file}: {e}", file=sys.stderr)
            return

        call_pattern = (
            r'(?:^|[\s({,=.]|->)'
            r'('
            r'(?:[a-zA-Z_][a-zA-Z0-9_]*(?:<[^>]*>)?::)*'
            r'(?:~?[a-zA-Z_][a-zA-Z0-9_]*(?:<[^>]*>)?|operator[^\s(]*)'
            r')'
            r'\s*\('
        )

        current_function = None
        function_def_line = -1
        brace_depth = 0
        i = 0

        while i < len(lines):
            line = lines[i]
            stripped = line.strip()

            is_cxx_candidate = '::' in stripped and ')' in stripped
            is_c_candidate = (
                '::' not in stripped
                and '(' in stripped
                and stripped.endswith(')')
                and not line[0:1].isspace()
                and not stripped.startswith('[')
                and not stripped.startswith('<')
            )

            if is_cxx_candidate or is_c_candidate:
                has_brace = '{' in stripped
                brace_line = line
                if not has_brace and i + 1 < len(lines):
                    if '{' in lines[i + 1]:
                        brace_line = lines[i + 1]
                        has_brace = True

                if has_brace:
                    func_match = re.search(
                        r'((?:[a-zA-Z_][a-zA-Z0-9_]*(?:<[^>]*>)?::)*~?[a-zA-Z_][a-zA-Z0-9_]*(?:<[^>]*>)?)\s*\(',
                        stripped
                    )
                    if func_match:
                        func_name = func_match.group(1).strip()
                        current_function = self.functions.get(func_name)
                        function_def_line = i
                        brace_depth = 0

            if current_function is not None and i >= current_function.start_line:
                brace_depth += stripped.count('{') - stripped.count('}')

            if current_function is not None and i != function_def_line:
                for match in re.finditer(call_pattern, stripped):
                    call_name = match.group(1).strip()
                    if call_name.endswith('operator'):
                        complete_name = self._complete_operator_name(call_name, line)
                        if complete_name != call_name:
                            call_name = complete_name
                    if not self._is_valid_function_call(call_name, stripped, match.start()):
                        continue
                    if call_name == current_function.name:
                        continue

                    source_info = self._extract_source_info(line)
                    call = FunctionCall(
                        name=call_name,
                        line_number=i,
                        source_file=source_info[0],
                        source_line=source_info[1],
                        gimple_file=str(gimple_file)
                    )
                    current_function.calls.append(call)
                    self.all_calls.append((current_function.name, call))

                # Virtual dispatch via OBJ_TYPE_REF — resolved inline (vtables ready)
                if 'OBJ_TYPE_REF' in stripped:
                    vc_match = re.search(
                        r'OBJ_TYPE_REF\s*\([^;]+;\s*\(struct\s+(\w+)\)[^)]*->(\d+)B\)',
                        stripped
                    )
                    if vc_match:
                        iface = vc_match.group(1)
                        token = int(vc_match.group(2))
                        impls = self._resolve_virtual(iface, token)
                        placeholder = f"{iface}::virtual_call[{token}]"
                        for impl in (impls if impls else [placeholder]):
                            if impl == current_function.name:
                                continue
                            source_info = self._extract_source_info(line)
                            call = FunctionCall(
                                name=impl,
                                line_number=i,
                                source_file=source_info[0],
                                source_line=source_info[1],
                                gimple_file=str(gimple_file)
                            )
                            current_function.calls.append(call)
                            self.all_calls.append((current_function.name, call))

            if current_function is not None and i > current_function.start_line:
                if brace_depth <= 0:
                    current_function = None
                    function_def_line = -1
                    brace_depth = 0

            i += 1

    def _extract_source_info(self, line: str) -> Tuple[Optional[str], Optional[int]]:
        """Extract source file and line number from GIMPLE annotation"""
        # Pattern: [/path/to/file.cpp:123:4] or similar
        match = re.search(r'\[([^:\]]+):(\d+):\d+\]', line)
        if match:
            return match.group(1), int(match.group(2))
        return None, None


    def _complete_operator_name(self, incomplete_name: str, line: str) -> str:
        """
        Complete incomplete operator names.
        E.g., convert 'std::function<void()>::operator' to 'std::function<void()>::operator()'
        """
        # Find where this incomplete name appears in the original line
        # Strip source annotations first
        if '] ' in line:
            stripped = line[line.find(']')+1:]
        else:
            stripped = line
        
        # Look for the incomplete name in the stripped line
        pos = stripped.find(incomplete_name)
        if pos >= 0 and pos + len(incomplete_name) < len(stripped):
            # Check what comes after the incomplete name
            after_text = stripped[pos + len(incomplete_name):]
            # Extract operator symbols - typically 1-3 special characters
            operator_chars = ''
            for char in after_text:
                if char in '()[]<>=!+-*/%&|^~':
                    operator_chars += char
                else:
                    # Stop at whitespace or other non-operator chars
                    break
            
            if operator_chars:
                return incomplete_name + operator_chars
        
        return incomplete_name

    def _is_valid_function_call(self, func_name: str, context: str, pos: int) -> bool:
        """Check if the matched text is actually a function call"""
        keywords = {
            'if', 'else', 'for', 'while', 'switch', 'case', 'return',
            'try', 'catch', 'finally', 'throw', 'sizeof', 'typeof',
            'auto', 'const', 'static', 'extern', 'struct', 'class',
            'union', 'enum', 'typedef', 'void', 'int', 'float', 'double',
            'bool', 'char', 'long', 'short', 'unsigned', 'signed',
            'goto', 'continue', 'break', 'default'
        }
        
        # Also filter out GIMPLE pseudo-operations
        gimple_pseudo_ops = {
            'CLOBBER', 'OBJ_TYPE_REF', 'PARM_DECL', 'VAR_DECL', 'TYPE_DECL',
            'FIELD_DECL', 'FUNCTION_DECL', 'LABEL_DECL', 'CONST_DECL',
            'IMPORTED_DECL'
        }

        if func_name.lower() in keywords or func_name.upper() in gimple_pseudo_ops:
            return False

        # Avoid matching type casts and similar constructs
        if context[max(0, pos-1):pos] in ['<', '[']:
            return False

        # Filter out very short names that might be variables
        if len(func_name) < 2:
            return False

        # Avoid matching things that look like incomplete expressions
        if func_name.startswith('_') and func_name.endswith('_'):
            return False

        return True

    def _is_function_end(self, lines: List[str], current_idx: int, func_start: int) -> bool:
        """Check if we've found the actual end of a function"""
        # Look for matching brace depth
        depth = 0
        for i in range(func_start, current_idx + 1):
            depth += lines[i].count('{') - lines[i].count('}')
        return depth == 0

    def get_call_path(self, function_name: str, visited: Optional[Set[str]] = None,
                      prefix: str = '', max_depth: int = 50, show_sources: bool = False,
                      _expanded: Optional[Set[str]] = None) -> List[str]:
        """
        Get the call path for a function - all functions it calls.
        Returns a formatted list of calls with indentation showing the hierarchy.

        prefix    - string of '│  ' / '   ' segments built up as we recurse, so
                    vertical guide-lines align correctly at every depth level.
        visited   - per-branch set; detects cycles in the current call path.
        _expanded - globally shared set; a function whose subtree has already been
                    fully printed will show '[...]' instead of repeating.
        """
        if visited is None:
            visited = set()
        if _expanded is None:
            _expanded = set()

        if max_depth <= 0:
            return [f"{prefix}└─ [max depth reached]"]

        if function_name not in self.functions:
            return []

        if function_name in visited:
            return [f"{prefix}└─ [circular reference to {function_name}]"]

        # If this function's subtree was already fully printed in another branch,
        # don't repeat it - show a back-reference marker instead.
        if function_name in _expanded:
            return [f"{prefix}└─ [...]"]

        # Ensure this function's file has been fully parsed before reading its calls
        self._ensure_file_parsed(function_name)

        # Mark expanded BEFORE recursing so sibling calls to the same function collapse too.
        _expanded.add(function_name)
        visited.add(function_name)
        func = self.functions[function_name]
        result = []

        if not func.calls:
            return []

        # Deduplicate calls by name
        unique_calls = {}
        for call in func.calls:
            if call.name not in unique_calls:
                unique_calls[call.name] = call

        visible_calls = list(unique_calls.items())
        for idx, (call_name, call) in enumerate(visible_calls):
            source_info = ""
            if show_sources and call.source_file and call.source_line:
                source_info = f" [{call.source_file}:{call.source_line}]"

            is_last = (idx == len(visible_calls) - 1)
            connector = "└─ " if is_last else "├─ "
            # Children of a non-last item need a '│  ' guide; children of a last item get '   '
            child_prefix = prefix + ("   " if is_last else "│  ")

            result.append(f"{prefix}{connector}{call_name}{source_info}")

            # Recursively get calls for this function.
            # visited is COPIED so cycle detection is per-path.
            # _expanded is SHARED so each subtree is printed only once.
            sub_calls = self.get_call_path(call_name, visited.copy(), child_prefix,
                                           max_depth - 1, show_sources, _expanded)
            result.extend(sub_calls)

        return result

    def get_call_tree(self, function_name: str, show_sources: bool = False,
                      max_depth: int = 50) -> str:
        """Generate a formatted call tree for a function"""
        if function_name not in self.functions:
            return f"Function '{function_name}' not found in GIMPLE files"

        # Ensure the root function's file is parsed before inspecting its calls
        self._ensure_file_parsed(function_name)

        func = self.functions[function_name]
        result = []

        # Add GIMPLE file info only if sources are shown
        if show_sources and func.gimple_file:
            result.append(f"From GIMPLE: {func.gimple_file}\n")

        result.append("─" * 100)

        # Show function name as root of tree
        source_info = ""
        if show_sources and func.source_file and func.source_line:
            source_info = f" [{func.source_file}:{func.source_line}]"

        result.append(function_name + source_info)

        if not func.calls:
            result.append("  (no function calls)")
        else:
            call_paths = self.get_call_path(function_name, show_sources=show_sources,
                                            max_depth=max_depth)
            if call_paths:
                result.extend(call_paths)
            else:
                result.append("  (no resolvable function calls)")

        result.append("─" * 100)
        result.append(f"  (fully parsed {len(self._parsed_files)} of {len(self.gimple_files)} GIMPLE files)")

        return "\n".join(result)

    def get_reachable_functions(self, function_name: str, max_depth: int = 50) -> List[str]:
        """Return a sorted unique list of all functions reachable from function_name."""
        if function_name not in self.functions:
            return []

        reachable: Set[str] = set()
        queue: List[Tuple[str, int]] = [(function_name, 0)]

        while queue:
            name, depth = queue.pop()
            if name in reachable or depth > max_depth:
                continue
            reachable.add(name)
            self._ensure_file_parsed(name)
            func = self.functions.get(name)
            if func is None:
                continue
            for call in func.calls:
                if call.name not in reachable:
                    queue.append((call.name, depth + 1))

        reachable.discard(function_name)  # exclude the root itself
        return sorted(reachable)

    def get_all_functions(self) -> List[str]:
        """Get list of all functions found in the GIMPLE file"""
        return sorted(self.functions.keys())

    def search_functions(self, pattern: str) -> List[str]:
        """Search for functions matching a pattern (substring or regex)"""
        import re as regex_module
        try:
            compiled = regex_module.compile(pattern, regex_module.IGNORECASE)
            return sorted([name for name in self.functions.keys() if compiled.search(name)])
        except regex_module.error:
            # Fall back to substring matching
            return sorted([name for name in self.functions.keys() if pattern.lower() in name.lower()])


def gimple_files_to_vtable_obj_files(gimple_files: List[str]) -> List[str]:
    """
    Given a list of GIMPLE files, return the companion .obj files that may
    contain vtable definitions (ELF .rela.rodata._ZTV* sections).

    Two cases require scanning a companion obj:
      1. The GIMPLE file explicitly references a _ZTV symbol — these are
         translation units that assign a vptr (constructors of concrete classes).
      2. The GIMPLE file defines C++ class methods (contains '::' function
         definitions) but has NO _ZTV reference — these are "key function"
         translation units where GCC emits the vtable as pure ELF data without
         any GIMPLE representation of the _ZTV symbol.

    The companion .obj path is derived by:
      1. Stripping the '.006t.gimple' suffix
      2. Stripping the redundant duplicate extension (.cpp or .c that GCC appends)
      3. Appending '.obj'
    """
    ztv_re = re.compile(r'_ZTV')
    # Matches C++ qualified names (class methods), used to detect class-method
    # translation units that may have implicit vtable emission.
    cxx_func_re = re.compile(r'\b\w+::\w+')
    obj_files = []
    for gf in gimple_files:
        try:
            with open(gf, 'r', encoding='utf-8', errors='ignore') as f:
                content = f.read()
        except Exception:
            continue
        has_ztv = ztv_re.search(content)
        has_cxx = cxx_func_re.search(content)
        if not has_ztv and not has_cxx:
            continue
        # Derive companion .obj path
        base = gf[:-len('.006t.gimple')]   # strip '.006t.gimple'
        dot = base.rfind('.')
        if dot >= 0:
            base = base[:dot]              # strip duplicate extension
        obj = base + '.obj'
        if Path(obj).exists():
            obj_files.append(obj)
    return obj_files


def scan_obj_vtables(obj_files: List[str]) -> Dict[str, Dict[int, str]]:
    """
    Scan the given .obj files for vtable definitions.

    Uses readelf -rW to read ELF relocation sections named .rela.rodata._ZTV*.
    Each relocation entry at byte offset O (where O >= 8) corresponds to vtable
    slot index (O - 8) / 4, because the first 8 bytes of a vtable are the
    offset-to-top and RTTI pointer (2 x 4-byte words on 32-bit targets).

    Mangled symbol names are demangled in bulk via c++filt.

    Returns:
        dict mapping demangled vtable name -> {slot_index -> demangled_func_name}
    """
    if not obj_files:
        return {}

    # Collect all mangled names so we can demangle in one c++filt call
    # vtables_raw: mangled_vtable_name -> {slot -> mangled_func}
    vtables_raw: Dict[str, Dict[int, str]] = {}
    mangled_names: Set[str] = set()

    rela_section_re = re.compile(
        r"Relocation section '\.rela\.rodata\.(_ZTV[^']+)'"
    )
    reloc_entry_re = re.compile(
        r'^([0-9a-fA-F]+)\s+[0-9a-fA-F]+\s+\S+\s+[0-9a-fA-F]+\s+(\S+)'  # offset ... symbol
    )

    for obj_file in sorted(obj_files):
        try:
            result = subprocess.run(
                ['readelf', '-rW', obj_file],
                capture_output=True, text=True, errors='ignore'
            )
            output = result.stdout
        except Exception:
            continue

        current_vtable = None
        for line in output.splitlines():
            m = rela_section_re.search(line)
            if m:
                current_vtable = m.group(1)
                if current_vtable not in vtables_raw:
                    vtables_raw[current_vtable] = {}
                    mangled_names.add(current_vtable)
                continue

            if current_vtable is None:
                continue

            # Stop parsing entries when we hit the next section header or blank
            if line.startswith('Relocation section') or line.startswith('There are no'):
                current_vtable = None
                continue

            em = reloc_entry_re.match(line.strip())
            if em:
                byte_offset = int(em.group(1), 16)
                if byte_offset < 8:
                    continue  # skip offset-to-top and RTTI pointer slots
                slot = (byte_offset - 8) // 4
                sym = em.group(2).rstrip('+')
                # Strip addend notation like "symbol + 0"
                sym = sym.split('+')[0].strip()
                if not sym or sym.startswith('.'):
                    continue
                if slot not in vtables_raw[current_vtable]:
                    vtables_raw[current_vtable][slot] = sym
                    mangled_names.add(sym)

    if not vtables_raw:
        return {}

    # Demangle all names in one shot
    mangled_list = sorted(mangled_names)
    try:
        result = subprocess.run(
            ['c++filt'],
            input='\n'.join(mangled_list),
            capture_output=True, text=True
        )
        demangled_list = result.stdout.strip().splitlines()
    except Exception:
        demangled_list = mangled_list

    demangle_map = dict(zip(mangled_list, demangled_list))

    def strip_params(name: str) -> str:
        """
        Strip the parameter list from a demangled function name so it matches
        the key format used by the GIMPLE parser (qualified name only, no parens).
        e.g. 'Foo::Bar::GetTick()' -> 'Foo::Bar::GetTick'
             'Foo::Bar<int>::Execute(float)' -> 'Foo::Bar<int>::Execute'
        """
        last_sep = name.rfind('::')
        if last_sep < 0:
            paren = name.find('(')
            return name[:paren] if paren >= 0 else name
        suffix = name[last_sep + 2:]
        paren = suffix.find('(')
        return name[:last_sep + 2 + paren] if paren >= 0 else name

    # Build final map with demangled names; strip parameter lists from function
    # names so they match the keys used by the GIMPLE parser.
    vtables: Dict[str, Dict[int, str]] = {}
    for mangled_vtable, slots in vtables_raw.items():
        demangled_vtable = demangle_map.get(mangled_vtable, mangled_vtable)
        vtable_key = demangled_vtable.replace('vtable for ', '')
        vtables[vtable_key] = {
            slot: strip_params(demangle_map.get(func, func))
            for slot, func in slots.items()
        }

    return vtables


def find_gimple_files(pattern: str = None, root_dir: str = None) -> List[str]:
    """Find all GIMPLE files matching the pattern"""
    if root_dir is None:
        root_dir = "."
    
    if pattern is None:
        pattern = "**/*.006t.gimple"
    else:
        pattern = f"**/{pattern}"
    
    gimple_files = glob.glob(str(Path(root_dir) / pattern), recursive=True)
    return sorted(gimple_files)


def print_help():
    """Print help message"""
    print("""
GIMPLE Call Path Tracer
=======================

Usage: python gimple_call_tracer.py [options]

Options:
  --dir <path>         Root directory to search for GIMPLE files (default: current dir)
  --function <name>    Show call graph (tree) for specified function
  --unique             With --function: show a flat sorted list of all reachable functions
                       instead of the call graph
  --list               List all functions found
  --search <pattern>   Search for functions (substring or regex)
  --stats              Show statistics about the GIMPLE files
  --scan-vtables       Scan .obj files for vtable definitions and print slot->function maps
  --max-depth <N>      Maximum depth for call tree (default: 50)
  --with-sources       Include source file and line number information
  --help               Show this message

Examples:
  python gimple_call_tracer.py --dir . --list
  python gimple_call_tracer.py --dir . --function "MyClass::myMethod"
  python gimple_call_tracer.py --dir . --function "MyClass::myMethod" --with-sources
  python gimple_call_tracer.py --dir . --search "Service"
  python gimple_call_tracer.py --dir . --function "Calibrate" --stats --max-depth 20
    """)


def main():
    if '--help' in sys.argv:
        print_help()
        return

    # Parse arguments
    root_dir = "."
    if '--dir' in sys.argv:
        idx = sys.argv.index('--dir')
        if idx + 1 < len(sys.argv):
            root_dir = sys.argv[idx + 1]

    max_depth = 50
    if '--max-depth' in sys.argv:
        idx = sys.argv.index('--max-depth')
        if idx + 1 < len(sys.argv):
            try:
                max_depth = int(sys.argv[idx + 1])
            except ValueError:
                print("Error: --max-depth must be an integer", file=sys.stderr)
                return

    # Check if source info should be displayed
    show_sources = '--with-sources' in sys.argv

    # Find all GIMPLE files
    print(f"Searching for GIMPLE files in: {root_dir}")
    gimple_files = find_gimple_files(root_dir=root_dir)

    if not gimple_files:
        print(f"No GIMPLE files found in {root_dir}", file=sys.stderr)
        return

    print(f"Found {len(gimple_files)} GIMPLE files")
    print("Fast-scanning GIMPLE files for function names and vtable data...")
    parser = GimpleParser(gimple_files)
    print(f"  {len(parser._vtable_obj_candidates)} companion .obj file(s) scanned for vtables")
    print(f"  Found {len(parser.vtables)} vtable(s), {len(parser.vptr_map)} interface vptr mappings resolved")
    print(f"  {len(parser.functions)} functions discovered across {len(gimple_files)} GIMPLE files\n")

    # Handle various options
    if '--scan-vtables' in sys.argv:
        if not parser.vtables:
            print("  No vtable definitions found in .obj files.")
        else:
            print(f"\nVtable definitions ({len(parser.vtables)} total):\n")
            for vtable_name in sorted(parser.vtables):
                slots = parser.vtables[vtable_name]
                funcs = [f"[{s}] {slots[s]}" for s in sorted(slots)]
                print(f"  VTABLE: {vtable_name}")
                for f in funcs:
                    print(f"    {f}")
                print()

    if '--list' in sys.argv:
        print("Parsing all function bodies for listing...")
        parser._ensure_all_parsed()
        print("Functions found in GIMPLE files:")
        print("-" * 100)
        for func_name in parser.get_all_functions():
            func = parser.functions[func_name]
            source_info = ""
            if func.source_file and func.source_line:
                source_info = f" [{func.source_file}:{func.source_line}]"
            print(f"  {func_name}{source_info}")
            if len(func.calls) > 0:
                unique_calls = len(set(call.name for call in func.calls))
                print(f"    Direct calls: {unique_calls} (call sites: {len(func.calls)})")

    if '--search' in sys.argv:
        idx = sys.argv.index('--search')
        if idx + 1 < len(sys.argv):
            pattern = sys.argv[idx + 1]
            results = parser.search_functions(pattern)
            print(f"\nFunctions matching '{pattern}':")
            print("-" * 100)
            if not results:
                print("  No matches found")
            for func_name in results:
                func = parser.functions[func_name]
                source_info = ""
                if func.source_file and func.source_line:
                    source_info = f" [{func.source_file}:{func.source_line}]"
                print(f"  {func_name}{source_info}")

    if '--function' in sys.argv:
        idx = sys.argv.index('--function')
        if idx + 1 < len(sys.argv):
            func_name = sys.argv[idx + 1]
            if func_name not in parser.functions:
                print(f"Function '{func_name}' not found in GIMPLE files")
            else:
                if '--unique' in sys.argv:
                    # Flat sorted list of all reachable functions
                    reachable = parser.get_reachable_functions(func_name, max_depth=max_depth)
                    print(f"{len(reachable)} functions reachable from '{func_name}':")
                    print("-" * 100)
                    for name in reachable:
                        source_info = ""
                        if show_sources:
                            func = parser.functions.get(name)
                            if func and func.source_file and func.source_line:
                                source_info = f" [{func.source_file}:{func.source_line}]"
                        print(f"  {name}{source_info}")
                    print(f"\n  (fully parsed {len(parser._parsed_files)} of {len(gimple_files)} GIMPLE files)")
                else:
                    # Call graph (tree) form
                    print(parser.get_call_tree(func_name, show_sources=show_sources,
                                               max_depth=max_depth))

    if '--stats' in sys.argv:
        parser._ensure_all_parsed()
        total_calls = sum(len(func.calls) for func in parser.functions.values())
        unique_call_targets = len(set(call.name for func in parser.functions.values() for call in func.calls))
        
        if parser.functions:
            max_calls_func = max(parser.functions.values(), key=lambda f: len(f.calls))
            max_calls = (len(max_calls_func.calls), max_calls_func.name)
        else:
            max_calls = (0, "N/A")
        
        print("\nStatistics:")
        print("-" * 100)
        print(f"  Total GIMPLE files: {len(gimple_files)}")
        print(f"  Total functions: {len(parser.functions)}")
        print(f"  Total function call sites: {total_calls}")
        print(f"  Unique function call targets: {unique_call_targets}")
        print(f"  Function with most calls: {max_calls[1]} ({max_calls[0]} call sites)")
        if parser.functions:
            print(f"  Average call sites per function: {total_calls / len(parser.functions):.2f}")


if __name__ == '__main__':
    main()
