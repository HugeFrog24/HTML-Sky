MAKEFLAGS += -s -j

DIST_DIR = ./dist
SRC_DIR = ./src

DEF = $(SRC_DIR)/proxy/winhttp-proxy.def

SRC_DIRS = $(SRC_DIR) $(wildcard $(SRC_DIR)/*/)

C_SRC = $(wildcard $(SRC_DIR)/*.c $(SRC_DIR)/*/*.c)
CPP_SRC = $(wildcard $(SRC_DIR)/*.cpp $(SRC_DIR)/*/*.cpp)

C_OBJ = $(addprefix $(DIST_DIR)/, $(notdir $(C_SRC:.c=.o)))
CPP_OBJ = $(addprefix $(DIST_DIR)/, $(notdir $(CPP_SRC:.cpp=.o)))

# .hpp as well as .h. It used to be .h only, so htinternal.hpp and
# modinspect.hpp were dependencies of nothing: editing the shared inspection
# policy rebuilt whatever .cpp changed and left every other object stale,
# linking two different versions of the policy into one DLL. Coarse on purpose -
# any header change rebuilds every object - because that is obviously correct,
# and this tree's vpath setup makes a generated .d scheme easy to get subtly
# wrong for no gain at this size.
CXX_HEADER = $(wildcard $(SRC_DIR)/*.h $(SRC_DIR)/*/*.h \
	$(SRC_DIR)/*.hpp $(SRC_DIR)/*/*.hpp)

TARGET = winhttp.dll
BIN_TARGET = $(DIST_DIR)/$(TARGET)

# Compiler paths.
CC = gcc
CXX = g++

# Params.
# One shared list, so a warning flag or a -Wno- suppression added for one
# variant also reaches the other. Kept apart they drift, and the build used to
# chase a bug ends up disagreeing with the shipped one about what compiles.
CFLAGS = -Wall -Wformat -Wno-unused-function -Wno-stringop-overread -ffunction-sections -fdata-sections -static
# DEBUG=1 keeps symbols and frame pointers, so a hang can be diagnosed by
# attaching gdb and reading a stack instead of guessing. The release default is
# unchanged: -O3, LTO and stripped.
ifeq ($(DEBUG),1)
# No -flto here: it inlines across translation units and flattens exactly the
# stack traces this variant exists to produce.
CFLAGS += -O1 -g -fno-omit-frame-pointer
# The loader is full of LOG() calls that compile to nothing unless this is set.
# Turning them on with the debug build means a startup hang can be located by
# reading html-log.log for the last line reached, without attaching a debugger -
# which matters here because the target is protected by WinLicense and an
# attach may be detected or refused outright.
CFLAGS += -DHTML_ENABLE_LOGGER
else
CFLAGS += -O3 -flto=auto -s
endif
CFLAGS += -I./src
# System import libraries. Named once and referenced twice below, because ld
# resolves left to right and libimgui.a - linked after these - reaches back for
# ImmGetContext and GetDeviceCaps.
SYSLIBS = -lgdi32 -ldwmapi -ld3dcompiler -lstdc++ -limm32
LFLAGS = -Wl,--gc-sections,-O3,--version-script,$(SRC_DIR)/exports.txt,--out-implib,$(DIST_DIR)/htmodloader.lib
LFLAGS += $(SYSLIBS)
# Include ImGui.
CFLAGS += -I./libraries/imgui-1.92.2b -I./libraries/imgui-1.92.2b/backends
LFLAGS += -L./libraries/imgui-1.92.2b -limgui -limgui_impl_win32 -limgui_impl_vulkan -limgui_impl_opengl3
# Include MinHook.
CFLAGS += -I./libraries/MinHook/include
LFLAGS += -L./libraries/MinHook -lMinHook
# Include Vulkan.
CFLAGS += -I./libraries/vulkan/Include
LFLAGS += -L./libraries/vulkan/Lib -lvulkan-1
# Include cJSON.
CFLAGS += -I./libraries/cJSON
LFLAGS += -L./libraries/cJSON -lcjson
# Include LevelDB.
CFLAGS += -I./libraries/leveldb/include
LFLAGS += -L./libraries/leveldb/lib -lleveldb -lz

# The second reference, after every static archive. The release build happened
# to get away with only the first; any change to the optimisation flags exposed
# it as a wall of undefined references unrelated to whatever was changed.
LFLAGS += $(SYSLIBS)
# Macros.
CFLAGS += -DNDEBUG -DHTMLAPIATTR=__declspec(dllexport)

vpath %.c $(SRC_DIRS)
vpath %.cpp $(SRC_DIRS)

.PHONY: all clean libs clean_libs clean_all scanner test clean_test

# The launcher's read-only scanner, built from the SAME ModInspect core the DLL
# links. That is the entire reason it exists: a launcher that installs and
# removes mods has to agree with the loader about what a mod IS, and a second
# implementation in another language can only discover a disagreement after one
# has already shipped.
#
# It lives outside src/ deliberately. The object lists above glob src/*.cpp and
# src/*/*.cpp, so a main() placed under src/ would be linked into winhttp.dll.
SCANNER_TARGET = $(DIST_DIR)/htmodscan.exe
SCANNER_SRC = ./scanner/main.cpp $(SRC_DIR)/modinspect.cpp \
	$(SRC_DIR)/utils/semver.cpp $(SRC_DIR)/utils/path.cpp

# The scanner's rule is further down on purpose: make takes the FIRST target in
# the file as the default goal, and putting it here quietly made a bare `make`
# build the scanner instead of the DLL.

$(BIN_TARGET): $(C_OBJ) $(CPP_OBJ)
	@echo Linking ...
	@$(CXX) --std=c++17 $(CFLAGS) $^ -shared -o $@ $(LFLAGS)
	@echo Done.

$(DIST_DIR)/%.o: %.c $(CXX_HEADER)
	@echo Compiling file "$<" ...
	@$(CC) --std=c11 $(CFLAGS) -c $< -o $@

$(DIST_DIR)/%.o: %.cpp $(CXX_HEADER)
	@echo Compiling file "$<" ...
	@$(CXX) $(CFLAGS) -c $< -o $@

$(DIST_DIR):
	-@mkdir dist

clean_all: clean_libs clean

clean:
	-@del .\dist\*.o
	-@del .\dist\*.dll
	-@del .\dist\*.lib

all: $(DIST_DIR) libs
	-@$(MAKE) $(BIN_TARGET)

scanner: $(DIST_DIR) libs
	@echo Building scanner ...
	@$(CXX) --std=c++17 $(CFLAGS) -municode $(SCANNER_SRC) \
		./libraries/cJSON/cJSON.o -o $(SCANNER_TARGET)
	@echo Done.

# Conformance tests for the shared inspection core.
#
# The fixture DLLs are built here, with the SAME compiler that builds the
# loader, because what is under test is what Win32 reports about a real PE. A
# hand-written byte array pretending to be a DLL would test the parser against
# the author's idea of a PE rather than against one the toolchain emits - and
# the bug this suite exists for (1812 on a PE with no .rsrc section at all) is
# invisible to any fixture that was not produced that way.
TEST_DIR = ./test
TEST_FIX = $(TEST_DIR)/fixtures
TEST_OUT = $(DIST_DIR)/test
TEST_TARGET = $(TEST_OUT)/modinspect_test.exe
TEST_SRC = $(TEST_DIR)/modinspect_test.cpp $(SRC_DIR)/modinspect.cpp \
	$(SRC_DIR)/utils/semver.cpp $(SRC_DIR)/utils/path.cpp

# Both forms on purpose: which shell make picks here depends on whether an sh is
# on PATH, and the two disagree about backslashes. Each is allowed to fail.
$(TEST_OUT):
	-@mkdir -p $(TEST_OUT)
	-@mkdir $(subst /,\,$(TEST_OUT))

test: $(DIST_DIR) $(TEST_OUT) libs
	@echo Building test fixtures ...
	@$(CC) -shared -o $(TEST_OUT)/noresource.dll $(TEST_FIX)/stub.c
	@windres -I$(TEST_FIX) $(TEST_FIX)/withmanifest.rc $(TEST_OUT)/withmanifest.res.o
	@$(CC) -shared -o $(TEST_OUT)/withmanifest.dll $(TEST_FIX)/stub.c $(TEST_OUT)/withmanifest.res.o
	@windres -I$(TEST_FIX) $(TEST_FIX)/secondmanifest.rc $(TEST_OUT)/secondmanifest.res.o
	@$(CC) -shared -o $(TEST_OUT)/secondmanifest.dll $(TEST_FIX)/stub.c $(TEST_OUT)/secondmanifest.res.o
	@windres -I$(TEST_FIX) $(TEST_FIX)/otherresource.rc $(TEST_OUT)/otherresource.res.o
	@$(CC) -shared -o $(TEST_OUT)/otherresource.dll $(TEST_FIX)/stub.c $(TEST_OUT)/otherresource.res.o
	@windres -I$(TEST_FIX) $(TEST_FIX)/broken.rc $(TEST_OUT)/broken.res.o
	@$(CC) -shared -o $(TEST_OUT)/broken.dll $(TEST_FIX)/stub.c $(TEST_OUT)/broken.res.o
	@echo Building tests ...
	@$(CXX) --std=c++17 $(CFLAGS) -municode $(TEST_SRC) \
		./libraries/cJSON/cJSON.o -o $(TEST_TARGET)
	@echo Running tests ...
	@$(TEST_TARGET) $(TEST_OUT) $(TEST_OUT)/scratch

clean_test:
	-@del .\dist\test\*.o .\dist\test\*.dll .\dist\test\*.exe

libs:
	@echo Compiling libraries ...
	-@$(MAKE) -s -C ./libraries/imgui-1.92.2b all
	-@$(MAKE) -s -C ./libraries/MinHook libMinHook.a
	-@$(MAKE) -s -C ./libraries/cJSON libcjson.a
	-@$(MAKE) -s -C ./libraries/leveldb all

clean_libs:
	-@$(MAKE) -s -C ./libraries/imgui-1.92.2b clean
	-@$(MAKE) -s -C ./libraries/MinHook clean
	-@$(MAKE) -s -C ./libraries/cJSON clean
