#!/usr/bin/env python3
# build-angle-ps5.py — ANGLE (GLES on Vulkan) as static libraries for our PS5 title.
# ANGLE builds with gn, which can't aim at the PS5: a gn "reference" build for Linux with the
# PS5's choices (no X11 / Wayland, the Vulkan display path, the system C++ library) lists every
# compile; here each one is made again with the PS5's compiler, then put in two archives.
#
#   python3 third_party/build-angle-ps5.py [--jobs 8] [--only <part of a path>]
#
# ~/ps5/angle (the source, gclient sync'd), ~/ps5/angle/out/ps5ref (the reference), out:
# ~/ps5/angle/out/ps5 (objects, libangle_ps5.a, failures.txt)

import os, re, shlex, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

HOME = os.path.expanduser('~')
ANGLE = os.environ.get('ANGLE_SRC', f'{HOME}/ps5/angle')
REF = f'{ANGLE}/out/ps5ref'
OUT = f'{ANGLE}/out/ps5'
HERE = os.path.dirname(os.path.abspath(__file__))
TOOLCHAIN = os.path.join(os.path.dirname(HERE), 'title', 'toolchain')
CC, CXX = f'{TOOLCHAIN}/ps5-cc', f'{TOOLCHAIN}/ps5-c++'
SDK = os.environ.get('PS5_PAYLOAD_SDK', f'{HOME}/ps5/PS5_Vulkan/.deps/native/ps5-payload-sdk')
AR = f'{SDK}/bin/prospero-ar'

# Linux-only defines of the reference build, not the PS5's
DROP_DEFINES = {'_FORTIFY_SOURCE', '_GLIBCXX_ASSERTIONS', 'USE_UDEV', 'USE_AURA', 'USE_GLIB', 'USE_OZONE',
                'CR_CLANG_REVISION', '__DATE__', '__TIME__', '__TIMESTAMP__', 'UNSAFE_BUFFERS_BUILD',
                'GPU_INFO_USE_LIBPCI'}
# sources that are Linux's own or not needed on the PS5 (we give ANGLE RADV's vkGetInstanceProcAddr)
SKIP = ('third_party/vulkan-loader/', 'SystemInfo_libpci.cpp')


def compiles():
    out = subprocess.run([f'{ANGLE}/third_party/ninja/ninja', '-C', REF, '-t', 'commands', 'libGLESv2', 'libEGL'],
                         capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        if ' -c ' not in line:
            continue
        args = shlex.split(line)
        if '-o' not in args or not args[0].endswith(('clang', 'clang++')):
            continue  # (a code generator's step, not a compile)
        src = args[args.index('-c') + 1]
        obj = args[args.index('-o') + 1]
        if any(s in src for s in SKIP):
            continue
        keep = []
        i = 1
        while i < len(args):
            a = args[i]
            if a.startswith('-D'):
                name = a[2:].split('=')[0]
                if name not in DROP_DEFINES:
                    keep.append(a)
            elif a in ('-I', '-isystem'):
                keep += [a, args[i + 1]]
                i += 1
            elif a.startswith('-I') or a.startswith('-std='):
                keep.append(a)
            elif a.startswith('-isystem'):
                keep.append(a)
            i += 1
        cxx = src.endswith(('.cpp', '.cc', '.cxx'))
        yield src, obj, cxx, keep


def build_one(job):
    src, obj, cxx, keep = job
    dst = os.path.join(OUT, obj)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    srcp = os.path.normpath(os.path.join(REF, src))
    if os.path.exists(dst) and os.path.getmtime(dst) > os.path.getmtime(srcp):
        return src, 0, ''
    cmd = [CXX if cxx else CC, '-O2', '-march=znver2', '-fPIC', '-ffunction-sections', '-fdata-sections',
           '-fvisibility=hidden', '-w', '-DANGLE_PLATFORM_PS5=1', '-DCLOCK_MONOTONIC_RAW=CLOCK_MONOTONIC'] + \
          (['-fno-exceptions', '-fno-rtti'] if cxx else []) + keep + ['-c', srcp, '-o', dst]
    # the reference's include paths are relative to its out dir
    r = subprocess.run(cmd, cwd=REF, capture_output=True, text=True)
    return src, r.returncode, r.stderr


def main():
    subprocess.run([sys.executable, os.path.join(HERE, 'patches', 'angle-ps5.py'), ANGLE], check=True)
    jobs = int(sys.argv[sys.argv.index('--jobs') + 1]) if '--jobs' in sys.argv else 8
    only = sys.argv[sys.argv.index('--only') + 1] if '--only' in sys.argv else None
    todo = [j for j in compiles() if not only or only in j[0]]
    print(f'{len(todo)} files')
    fails = []
    with ThreadPoolExecutor(jobs) as ex:
        for n, (src, rc, err) in enumerate(ex.map(build_one, todo), 1):
            if rc:
                fails.append((src, err))
            if n % 50 == 0:
                print(f'  {n}/{len(todo)}, {len(fails)} failed', flush=True)
    with open(f'{OUT}/failures.txt', 'w') as f:
        for src, err in fails:
            f.write(f'==== {src}\n{err}\n')
    # the first error of each failed file, grouped: what to fix first
    kinds = {}
    for src, err in fails:
        m = re.search(r'error: (.*)', err)
        k = m.group(1)[:140] if m else err.strip()[:140]
        kinds.setdefault(k, []).append(src)
    print(f'{len(todo) - len(fails)} built, {len(fails)} failed')
    for k, v in sorted(kinds.items(), key=lambda kv: -len(kv[1]))[:25]:
        print(f'  {len(v):4d}  {k}   (e.g. {v[0]})')
    if not fails and not only:
        # not the exported gl* / egl* names (ps5-opengl has them too): ANGLE's own GL_* / EGL_*
        # entry points, which EGL_GetProcAddress hands out by the usual names
        objs = [os.path.join(OUT, o) for _, o, _, _ in compiles()
                if '/libEGL/' not in o and not o.endswith('libGLESv2_autogen.o')]
        lib = f'{OUT}/libangle_ps5.a'
        if os.path.exists(lib):
            os.remove(lib)
        subprocess.run([AR, 'rcs', lib] + objs, check=True)
        print('archive:', lib)


if __name__ == '__main__':
    main()
