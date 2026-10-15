#!/usr/bin/env python3
"""The ABI boundary of VXRuntime's sources, read by clang itself (libclang through ctypes, as the x86-64 build sees
them): every function with the Windows calling convention (ms_abi: translated x86 code may call it, so the ARM64
build needs a thunk for it) and every call through an ms_abi function pointer (the runtime calling code that may be
x86: the ARM64 build must route it through the engine).

usage: vx_abi_scan.py <source.c|.m> ...   prints JSON: {file: {"functions": [...], "callbacks": [...]}}"""
import ctypes as C, json, os, subprocess, sys

LIB = "/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/lib/libclang.dylib"
lib = C.CDLL(LIB)

class Cursor(C.Structure): _fields_ = [("kind", C.c_int), ("xdata", C.c_int), ("data", C.c_void_p * 3)]
class Type(C.Structure): _fields_ = [("kind", C.c_int), ("data", C.c_void_p * 2)]
class String(C.Structure): _fields_ = [("data", C.c_void_p), ("flags", C.c_uint)]
class Loc(C.Structure): _fields_ = [("ptr", C.c_void_p * 2), ("int", C.c_uint)]

def fn(name, res, *args):
    f = getattr(lib, name); f.restype = res; f.argtypes = list(args); return f

createIndex = fn("clang_createIndex", C.c_void_p, C.c_int, C.c_int)
parseTU = fn("clang_parseTranslationUnit", C.c_void_p, C.c_void_p, C.c_char_p, C.POINTER(C.c_char_p), C.c_int, C.c_void_p, C.c_uint, C.c_uint)
tuCursor = fn("clang_getTranslationUnitCursor", Cursor, C.c_void_p)
VISITOR = C.CFUNCTYPE(C.c_int, Cursor, Cursor, C.c_void_p)
visit = fn("clang_visitChildren", C.c_uint, Cursor, VISITOR, C.c_void_p)
getLoc = fn("clang_getCursorLocation", Loc, Cursor)
fromMain = fn("clang_Location_isFromMainFile", C.c_int, Loc)
spellLoc = fn("clang_getSpellingLocation", None, Loc, C.c_void_p, C.POINTER(C.c_uint), C.POINTER(C.c_uint), C.POINTER(C.c_uint))
cursorType = fn("clang_getCursorType", Type, Cursor)
canon = fn("clang_getCanonicalType", Type, Type)
pointee = fn("clang_getPointeeType", Type, Type)
callConv = fn("clang_getFunctionTypeCallingConv", C.c_int, Type)
resultType = fn("clang_getResultType", Type, Type)
numArgs = fn("clang_getNumArgTypes", C.c_int, Type)
argType = fn("clang_getArgType", Type, Type, C.c_uint)
variadic = fn("clang_isFunctionTypeVariadic", C.c_uint, Type)
sizeOf = fn("clang_Type_getSizeOf", C.c_longlong, Type)
typeSpell = fn("clang_getTypeSpelling", String, Type)
cursorSpell = fn("clang_getCursorSpelling", String, Cursor)
referenced = fn("clang_getCursorReferenced", Cursor, Cursor)
isDef = fn("clang_isCursorDefinition", C.c_uint, Cursor)
storage = fn("clang_Cursor_getStorageClass", C.c_int, Cursor)
cursorArg = fn("clang_Cursor_getArgument", Cursor, Cursor, C.c_uint)
cursorNumArgs = fn("clang_Cursor_getNumArguments", C.c_int, Cursor)
cstr = fn("clang_getCString", C.c_char_p, String)
nullCursor = fn("clang_Cursor_isNull", C.c_int, Cursor)
numDiags = fn("clang_getNumDiagnostics", C.c_uint, C.c_void_p)
getDiag = fn("clang_getDiagnostic", C.c_void_p, C.c_void_p, C.c_uint)
diagSeverity = fn("clang_getDiagnosticSeverity", C.c_int, C.c_void_p)
formatDiag = fn("clang_formatDiagnostic", String, C.c_void_p, C.c_uint)
SDK = subprocess.run(["xcrun", "--show-sdk-path"], capture_output=True, text=True, check=True).stdout.strip()

def s(x): return (cstr(x) or b"").decode()
WIN64 = 10                                               # CXCallingConv_X86_64Win64
K_FUNCDECL, K_CALLEXPR, K_TYPEDEF, K_TU = 8, 103, 20, 300
K_POINTER, K_RECORD, K_ENUM, K_FPROTO, K_FNOPROTO, K_VOID, K_FLOAT, K_DOUBLE, K_LDOUBLE = 101, 105, 106, 111, 110, 2, 21, 22, 23
INTS = set(range(3, 21)) | {K_ENUM, K_POINTER, 102, 109, 27, 28, 29, 24, 112, 114}   # (arrays: parameters, so pointers)

def fntype(t):                                           # a function type behind pointers / typedefs, or None
    t = canon(t)
    while t.kind == K_POINTER: t = canon(pointee(t))
    return t if t.kind in (K_FPROTO, K_FNOPROTO) else None

def classify(t):                                         # how a value of type t crosses the Windows x64 ABI
    c = canon(t)
    if c.kind == K_VOID: return "void"
    if c.kind == K_FLOAT: return "float"
    if c.kind == K_DOUBLE: return "double"
    if c.kind in INTS: return "int"
    if c.kind == K_RECORD:
        n = sizeOf(c)
        return f"rec{n}" if n in (1, 2, 4, 8) else f"recref{n}"   # (by value in a slot, else a pointer to a copy)
    return f"?{c.kind}"

def desc(t):                                             # spelling, ABI class, canonical kind and size of a type
    c = canon(t)
    return {"type": s(typeSpell(t)), "cls": classify(t), "kind": c.kind, "size": sizeOf(c) if c.kind != K_VOID else 0}

def where(cur):
    line, col, off = C.c_uint(), C.c_uint(), C.c_uint()
    spellLoc(getLoc(cur), None, C.byref(line), C.byref(col), C.byref(off))
    return line.value, col.value

def scan(path, index):
    args = [b"-target", b"x86_64-apple-macos14", b"-isysroot", SDK.encode(), b"-fobjc-arc", b"-Isrc", b"-Wno-everything"]
    if path.endswith(".m"): args = [b"-x", b"objective-c"] + args
    argv = (C.c_char_p * len(args))(*args)
    tu = parseTU(index, path.encode(), argv, len(args), None, 0, 0x200)   # (KeepGoing)
    if not tu: raise SystemExit(f"cannot parse {path}")
    errors = [s(formatDiag(getDiag(tu, i), 0)) for i in range(numDiags(tu)) if diagSeverity(getDiag(tu, i)) >= 3]
    if errors: raise SystemExit(f"{path}: {len(errors)} errors, first: {errors[0]}")
    funcs, calls, seen = [], [], set()

    def visitor(cur, parent, _):
        if parent.kind == K_TU and not fromMain(getLoc(cur)): return 1          # (headers: skipped)
        if cur.kind == K_FUNCDECL and isDef(cur):
            t = cursorType(cur)
            if callConv(canon(t)) == WIN64:
                name = s(cursorSpell(cur))
                if name not in seen:
                    seen.add(name)
                    n = cursorNumArgs(cur)
                    params = [desc(cursorType(cursorArg(cur, i))) for i in range(max(n, 0))]
                    r = desc(resultType(t))
                    funcs.append({"name": name, "static": storage(cur) == 3, "line": where(cur)[0],
                                  "ret": r["type"], "ret_cls": r["cls"], "ret_kind": r["kind"], "ret_size": r["size"],
                                  "params": params, "variadic": bool(variadic(canon(t)))})
        elif cur.kind == K_CALLEXPR:
            ref = referenced(cur)
            if nullCursor(ref) or ref.kind != K_FUNCDECL:
                ft = None
                # the callee: the call's first child expression
                box = []
                def first(c, p, _):
                    box.append(Cursor(c.kind, c.xdata, (C.c_void_p * 3)(*c.data))); return 0
                visit(cur, VISITOR(first), None)
                if box: ft = fntype(cursorType(box[0]))
                if ft is not None and callConv(ft) == WIN64:
                    line, col = where(cur)
                    calls.append({"line": line, "col": col, "ret_cls": classify(resultType(ft)),
                                  "params": [classify(argType(ft, i)) for i in range(max(numArgs(ft), 0))],
                                  "callee": s(cursorSpell(ref)) if not nullCursor(ref) else ""})
        return 2
    visit(tuCursor(tu), VISITOR(visitor), None)
    return {"functions": funcs, "callbacks": calls}

def main():
    index = createIndex(0, 0)
    out = {}
    for p in sys.argv[1:]: out[p] = scan(p, index)
    json.dump(out, sys.stdout, indent=1)

if __name__ == "__main__": main()
