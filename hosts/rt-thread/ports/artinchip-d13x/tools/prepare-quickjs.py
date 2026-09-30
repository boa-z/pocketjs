"""Prepare a SHA256-pinned QuickJS-ng core, without quickjs-libc or threads."""
import hashlib
import io
import re
import tarfile
import urllib.request
from pathlib import Path
import portenv as pe

REVISION = "3c051980ab7e783dfbfb1c70c014ce5e05ecf24c"
ARCHIVE_SHA256 = "2faf39d4256801fcc1e33a3631c3e4a23b9e72889f8240d449142baea347a3e8"
SOURCE_SHA256 = "0dc62d8f9a2ed0e6f19fba948a1c54ee09c8d5c74545a814918716da89ccc35f"


def prepare(source):
    if hashlib.sha256(source).hexdigest() != SOURCE_SHA256:
        raise ValueError("QuickJS source identity changed; review patches")
    text = source.decode()
    start = text.index("static JSValue js_typed_array_reverse(")
    end = text.index("\nstatic ", start + 1)
    body = text[start:end]
    assert body.count("    if (len > 0) {") == 1
    body = body.replace("    if (len > 0) {", "    if (typed_array_is_immutable(JS_VALUE_GET_OBJ(this_val)))\n"
                        "        return JS_ThrowTypeErrorImmutableArrayBuffer(ctx);\n    if (len > 0) {", 1)
    text = text[:start] + body + text[end:]
    text, count = re.subn(r"(static JSValue js_typed_array___speciesCreate\([\s\S]*?JSValueConst \*argv)\)",
                          r"\1, bool writable)", text)
    assert count == 2
    text = text.replace("js_typed_array___speciesCreate(ctx, JS_UNDEFINED, 2, args)",
                        "js_typed_array___speciesCreate(ctx, JS_UNDEFINED, 2, args, true)")
    text = text.replace("js_typed_array___speciesCreate(ctx, JS_UNDEFINED, 4, args)",
                        "js_typed_array___speciesCreate(ctx, JS_UNDEFINED, 4, args, false)")
    start = text.rindex("static JSValue js_typed_array___speciesCreate(")
    end = text.index("\nstatic ", start + 1)
    body = text[start:end].replace("    return ret;", "    if (writable && !JS_IsException(ret) &&\n"
        "        typed_array_is_immutable(JS_VALUE_GET_OBJ(ret))) {\n"
        "        JS_FreeValue(ctx, ret);\n        return JS_ThrowTypeErrorImmutableArrayBuffer(ctx);\n    }\n    return ret;")
    text = text[:start] + body + text[end:]
    # Xuantie newlib uses long for int32_t despite sizeof(int) == 4.
    # Use the API's actual pointed-to type instead of relying on aliasing.
    for old, new in (
        ('int new_line_num, new_col_num, line_num, col_num, pc, v, ret;',
         'int new_line_num, new_col_num, line_num, col_num, pc, ret;\n    int32_t v;'),
        ('    int radix, flags;', '    int32_t radix;\n    int flags;'),
        ('    int remainingElementsCount;', '    int32_t remainingElementsCount;'),
        ('    int is_zero, index;', '    int is_zero;\n    int32_t index;'),
    ):
        if text.count(old) != 1:
            raise ValueError('QuickJS integer portability patch drift: ' + old)
        text = text.replace(old, new)
    return text.replace("#if !defined(__TINYC__) &&", "#if !defined(PJS_FREESTANDING) && !defined(__TINYC__) &&", 1)


def main():
    cache = pe.repo_root() / ".pocket-build/quickjs-v0.14.0.tar.gz"
    cache.parent.mkdir(parents=True, exist_ok=True)
    data = cache.read_bytes() if cache.exists() else urllib.request.urlopen(
        f"https://codeload.github.com/quickjs-ng/quickjs/tar.gz/{REVISION}", timeout=60).read()
    if hashlib.sha256(data).hexdigest() != ARCHIVE_SHA256:
        raise ValueError("QuickJS archive SHA256 mismatch")
    if not cache.exists():
        cache.write_bytes(data)
    dest = pe.repo_root() / ".pocket-build/quickjs"
    dest.mkdir(parents=True, exist_ok=True)
    with tarfile.open(fileobj=io.BytesIO(data)) as archive:
        for member in archive.getmembers():
            relative = Path(member.name).parts
            if not member.isfile() or len(relative) != 2:
                continue
            name = relative[1]
            if not (name.endswith(('.c', '.h')) or name == 'LICENSE'):
                continue
            content = archive.extractfile(member).read()
            if name == 'quickjs.c':
                content = prepare(content).encode()
            if name == 'cutils.h':
                text = content.decode().replace('!defined(_WIN32) && !defined(EMSCRIPTEN)',
                    '!defined(PJS_FREESTANDING) && !defined(_WIN32) && !defined(EMSCRIPTEN)')
                text = text.replace('#if defined(EMSCRIPTEN) || defined(__wasi__) || defined(__DJGPP)',
                    '#if defined(PJS_FREESTANDING) || defined(EMSCRIPTEN) || defined(__wasi__) || defined(__DJGPP)')
                text = text.replace('static inline uint64_t js__hrtime_ns(void) {\n#ifdef __DJGPP',
                    'static inline uint64_t js__hrtime_ns(void) {\n#ifdef PJS_FREESTANDING\n'
                    '  extern int64_t pjs_quickjs_time_us(void);\n  return pjs_quickjs_time_us() * 1000;\n#elif defined(__DJGPP)')
                text = text.replace('static inline int64_t js__gettimeofday_us(void) {\n',
                    'static inline int64_t js__gettimeofday_us(void) {\n#ifdef PJS_FREESTANDING\n'
                    '    extern int64_t pjs_quickjs_time_us(void);\n    return pjs_quickjs_time_us();\n#else\n')
                text = text.replace('return ((int64_t)tv.tv_sec * 1000000) + tv.tv_usec;\n}',
                    'return ((int64_t)tv.tv_sec * 1000000) + tv.tv_usec;\n#endif\n}')
                content = text.encode()
            target = dest / name
            if not target.exists() or target.read_bytes() != content:
                target.write_bytes(content)
    print(f"QuickJS-ng 0.14.0 {REVISION}: verified and prepared at {dest}")


if __name__ == '__main__':
    main()
