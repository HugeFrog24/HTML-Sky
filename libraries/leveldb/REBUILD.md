# Rebuilding libleveldb.a and libz.a

`lib/libleveldb.a` and `lib/libz.a` in this fork are **not** the archives
upstream ships. They were rebuilt, and this file is the recipe, because a
prebuilt binary nobody can reproduce is worse than no prebuilt binary.

## Why

Upstream's `libleveldb.a` carries 15 undefined references to `__gthr_win32_*`:

```
__gthr_win32_mutex_destroy
__gthr_win32_mutex_init_function
__gthr_win32_mutex_lock
__gthr_win32_mutex_unlock
```

Those come from libstdc++ built with GCC's **win32** thread model. A MinGW-w64
toolchain built with the **posix** thread model (winpthreads) does not export
them, so the link fails with a wall of undefined references that say nothing
about the actual problem. Check which one you have with `g++ -v | grep Thread`.

This is the same toolchain difference behind the `gMutex` change in
`src/backends/html_impl_vklayer.cpp` - see the comment there.

If you build with a win32-thread-model toolchain, upstream's archives link fine
and you do not need any of this.

## leveldb

Source: <https://github.com/extremeheat/leveldb-mcpe> at `1bcc6ae`
("add SHARED build option").

One patch is required. `util/Filepath.h` gates on `_MSC_VER` where it means
"Windows", in four places:

```diff
-#if defined(_MSC_VER)
+#if defined(_WIN32)
```

Without it `port::filepath` is `std::string` under MinGW while `util/env_win.cc`
uses `std::wstring`, and the build fails on the type mismatch.

```sh
git clone https://github.com/extremeheat/leveldb-mcpe
cd leveldb-mcpe
git checkout 1bcc6ae
sed -i 's/#if defined(_MSC_VER)/#if defined(_WIN32)/' util/Filepath.h
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
cmake --build build
cp build/libleveldb.a <this repo>/libraries/leveldb/lib/
```

## zlib

Source: <https://github.com/madler/zlib> at `51b7f2a` (1.3.1).
No patch needed.

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
cp build/libz.a <this repo>/libraries/leveldb/lib/libz.a
```

## Checking the result

The rebuilt archives should have no `__gthr_win32_*` undefined symbols and
should still need only zlib's `deflate`/`inflate` from outside themselves:

```sh
nm -u libraries/leveldb/lib/libleveldb.a | grep __gthr_win32   # expect nothing
```
