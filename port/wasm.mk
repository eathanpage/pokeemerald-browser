# Builds the browser (WebAssembly) port of pokeemerald.
# Run from the repository root:  make -f port/wasm.mk
# Requires: emsdk (emcc on PATH), arm-none-eabi binutils, python3.

EMCC     ?= emcc
PYTHON   ?= python3
PREPROC  := tools/preproc/preproc
DEBUG    ?= 0

ifeq ($(DEBUG),1)
WASM_DIR := build/wasm-debug
DIST_DIR := port/dist-debug
OPT      := -O1 -g
else
WASM_DIR := build/wasm
DIST_DIR := port/dist
OPT      := -O2 -g0
endif

# Game sources that are replaced by the port layer (port/src) or are
# GBA-hardware-only.
EXCLUDED_SRCS := \
	src/agb_flash.c src/agb_flash_1m.c src/agb_flash_le.c src/agb_flash_mx.c \
	src/siirtc.c src/libisagbprn.c \
	src/librfu_intr.c src/librfu_rfu.c src/librfu_sio32id.c src/librfu_stwi.c

GAME_SRCS := $(filter-out $(EXCLUDED_SRCS),$(wildcard src/*.c))
PORT_SRCS := $(wildcard port/src/*.c)

GAME_OBJS := $(patsubst src/%.c,$(WASM_DIR)/src/%.o,$(GAME_SRCS))
PORT_OBJS := $(patsubst port/src/%.c,$(WASM_DIR)/port/%.o,$(PORT_SRCS))

# Assembled GBA data (scripts, maps, songs...), converted to wasm objects.
# These come from the regular modern GBA build, which also generates the
# graphics and other assets the C files include.
ARM_DATA_OBJS := $(patsubst data/%.s,build/modern/data/%.o,$(wildcard data/*.s)) \
	$(patsubst sound/songs/midi/%.mid,build/modern/sound/songs/midi/%.o,$(wildcard sound/songs/midi/*.mid))
DATA_OBJS := $(WASM_DIR)/data/gba_data.o

DEFINES  := -DMODERN=1 -DPORT_WASM=1
CPPFLAGS := -iquote include -Iport/include -Wno-trigraphs $(DEFINES)
CFLAGS   := $(OPT) -std=gnu17 -funsigned-char -fno-strict-aliasing -fwrapv \
	-fno-delete-null-pointer-checks -Wno-everything

# Leave the bottom megabyte of linear memory to the emulated GBA memory map
# (see include/gba/defines.h).
LDFLAGS := $(OPT) --profiling-funcs -sGLOBAL_BASE=1048576 -sINITIAL_MEMORY=64MB -sALLOW_MEMORY_GROWTH=1 \
	-sSTACK_SIZE=1MB -sENVIRONMENT=web,node -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPU16,HEAPF32 \
	-sEXPORTED_FUNCTIONS=@port/exports.txt -sMODULARIZE=1 -sEXPORT_NAME=createEmerald \
	-sSUPPORT_LONGJMP=0

ifeq ($(DEBUG),1)
LDFLAGS += -sASSERTIONS=2
endif

.PHONY: all clean serve gba-build
.SECONDARY:
.DELETE_ON_ERROR:

WEB_FILES := $(patsubst port/web/%,$(DIST_DIR)/%,$(wildcard port/web/*))

all: $(DIST_DIR)/emerald.js $(WEB_FILES)

$(DIST_DIR)/%: port/web/%
	@mkdir -p $(@D)
	cp $< $@

# Serves the build on http://localhost:8000
serve: all
	cd $(DIST_DIR) && $(PYTHON) -m http.server 8000

# Runs the GBA build first (incremental, so cheap when up to date).
gba-build:
	@$(MAKE) -f Makefile modern

$(ARM_DATA_OBJS): | gba-build ;

$(WASM_DIR)/src/%.o: src/%.c | gba-build
	@mkdir -p $(@D)
	@echo "emcc $<"
	@$(EMCC) -E $(CPPFLAGS) -MMD -MP -MF $(@:.o=.d) -MT $@ $< | $(PREPROC) -i -g build/assets $< charmap.txt | $(EMCC) $(CFLAGS) -x c -c - -o $@

$(WASM_DIR)/port/%.o: port/src/%.c
	@mkdir -p $(@D)
	@echo "emcc $<"
	@$(EMCC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

# How the C code calls the functions that the data refers to (see
# port/tools/elf2wasm.py): specials are all called as u16 (*)(void), field
# effect script natives as u32 (*)(void), and so on.
CALL_SIGS := \
	--call-sig "gSpecials=() -> (i32)" \
	--call-sig "gScriptCmdTable=(i32) -> (i32)" \
	--call-sig "gMysteryEventScriptCmdTable=(i32) -> (i32)" \
	--call-sig "field_effect_scripts=() -> (i32)" \
	--call-sig "battle_anim_scripts=(i32) -> ()"

# The data converter needs to know the signatures of the C functions that the
# scripts reference, so it reads them from the compiled objects.
$(WASM_DIR)/data/gba_data.s: port/tools/elf2wasm.py $(ARM_DATA_OBJS) $(GAME_OBJS) $(PORT_OBJS)
	@mkdir -p $(@D)
	@echo "elf2wasm $@"
	@$(PYTHON) port/tools/elf2wasm.py -o $@ $(CALL_SIGS) --wasm-objs $(GAME_OBJS) $(PORT_OBJS) --elf-objs $(ARM_DATA_OBJS)

$(WASM_DIR)/data/gba_data.o: $(WASM_DIR)/data/gba_data.s
	$(EMCC) -c $< -o $@

$(DIST_DIR)/emerald.js: $(GAME_OBJS) $(PORT_OBJS) $(DATA_OBJS) port/exports.txt
	@mkdir -p $(@D)
	@echo "link $@"
	@$(EMCC) $(LDFLAGS) -o $@ $(GAME_OBJS) $(PORT_OBJS) $(DATA_OBJS)

-include $(GAME_OBJS:.o=.d) $(PORT_OBJS:.o=.d)

clean:
	rm -rf $(WASM_DIR) $(DIST_DIR)
