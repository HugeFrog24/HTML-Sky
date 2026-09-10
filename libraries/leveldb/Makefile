# leveldb, built from the vendored source in src/.
#
# WHY THIS IS BUILT RATHER THAN SHIPPED PREBUILT
#
# It used to be a committed libleveldb.a, and that archive silently encoded the
# thread model of whoever compiled it. A MinGW toolchain built --enable-threads
# =win32 produces an archive with undefined __gthr_win32_* references; one built
# --enable-threads=posix produces undefined pthread_* references instead. Each
# links against its own libstdc++ and fails against the other, with a wall of
# undefined references that says nothing about the real cause. Rebuilding the
# archive only moves which half of the world can build the loader.
#
# Compiling from source removes the question: the objects match whatever
# toolchain is present. imgui, MinHook and cJSON are already built this way by
# the top-level `libs` target - leveldb was the sole exception, and the sole
# dependency that ever broke a build here.
#
# THE SOURCE
#
# github.com/extremeheat/leveldb-mcpe at 1bcc6ae, with one change: util/
# Filepath.h gated on _MSC_VER where it meant "Windows", so port::filepath was
# std::string under MinGW while util/env_win.cc used std::wstring. It is
# _WIN32 here, in four places.
#
# zlib is not built here. lib/libz.a is prebuilt and carried by upstream, and
# unlike leveldb it is pure C with no thread-model dependency, so it links under
# either model. zlib/ holds the matching 1.3.1 headers so the compile and the
# link agree without depending on the toolchain shipping zlib.

CXX = g++
AR = ar

CXXFLAGS = -O3 -DNDEBUG -DLEVELDB_PLATFORM_WINDOWS "-DDLLX=__declspec(dllexport)"
CXXFLAGS += -I./src -I./src/include -I./zlib
CXXFLAGS += -Wno-deprecated-declarations

# Listed explicitly rather than by wildcard, because src/ is a faithful copy of
# upstream and carries 34 files that must NOT be compiled: unit tests,
# db_bench, and the POSIX/boost env and port backends. This list is the WIN32
# branch of upstream's CMakeLists.txt.
SRCS = \
  src/db/builder.cc src/db/c.cc src/db/db_impl.cc src/db/db_iter.cc \
  src/db/dbformat.cc src/db/filename.cc src/db/log_reader.cc \
  src/db/log_writer.cc src/db/memtable.cc src/db/repair.cc \
  src/db/table_cache.cc src/db/version_edit.cc src/db/version_set.cc \
  src/db/write_batch.cc \
  src/table/block.cc src/table/block_builder.cc src/table/filter_block.cc \
  src/table/format.cc src/table/iterator.cc src/table/merger.cc \
  src/table/table.cc src/table/table_builder.cc src/table/two_level_iterator.cc \
  src/util/arena.cc src/util/bloom.cc src/util/cache.cc src/util/coding.cc \
  src/util/comparator.cc src/util/crc32c.cc src/util/env.cc \
  src/util/filter_policy.cc src/util/hash.cc src/util/histogram.cc \
  src/util/logging.cc src/util/options.cc src/util/status.cc \
  src/db/zlib_compressor.cc src/db/zstd_compressor.cc \
  src/port/port_posix_sse.cc \
  src/port/port_win.cc src/util/env_win.cc src/util/win_logger.cc

OBJS = $(SRCS:.cc=.o)

all: lib/libleveldb.a

lib/libleveldb.a: $(OBJS)
	@$(AR) rcs $@ $^

.cc.o:
	@$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	-@del /q src\db\*.o src\table\*.o src\util\*.o src\port\*.o
	-@del lib\libleveldb.a

.PHONY: all clean
.SUFFIXES: .cc .o
