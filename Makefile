MAKEFLAGS += -s -j

DIST_DIR = ./dist
SRC_DIR = ./src

DEF = $(SRC_DIR)/proxy/winhttp-proxy.def

SRC_DIRS = $(SRC_DIR) $(wildcard $(SRC_DIR)/*/)

C_SRC = $(wildcard $(SRC_DIR)/*.c $(SRC_DIR)/*/*.c)
CPP_SRC = $(wildcard $(SRC_DIR)/*.cpp $(SRC_DIR)/*/*.cpp)

C_OBJ = $(addprefix $(DIST_DIR)/, $(notdir $(C_SRC:.c=.o)))
CPP_OBJ = $(addprefix $(DIST_DIR)/, $(notdir $(CPP_SRC:.cpp=.o)))

CXX_HEADER = $(wildcard $(SRC_DIR)/*.h $(SRC_DIR)/*/*.h)

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

.PHONY: all clean libs clean_libs clean_all

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

libs:
	@echo Compiling libraries ...
	-@$(MAKE) -s -C ./libraries/imgui-1.92.2b all
	-@$(MAKE) -s -C ./libraries/MinHook libMinHook.a
	-@$(MAKE) -s -C ./libraries/cJSON libcjson.a

clean_libs:
	-@$(MAKE) -s -C ./libraries/imgui-1.92.2b clean
	-@$(MAKE) -s -C ./libraries/MinHook clean
	-@$(MAKE) -s -C ./libraries/cJSON clean
