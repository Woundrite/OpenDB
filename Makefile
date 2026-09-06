#
# OpenDB Makefile (spec §6.2 + §6.3).
#
# Cross-platform: detects the compiler (g++ / clang++ / cl), picks C++23 mode,
# and falls back between POSIX-style and Windows-style directory creation.
# Compile strict warnings, treat warnings as errors.
#
# Targets:
#   make              -> build/opendb (REPL), build/test_runner (unit tests)
#   make run          -> build/opendb with stdin/stdout attached
#   make test         -> run the test runner
#   make smoke        -> run the milestone-5 end-to-end smoke test
#   make clean        -> wipe build/

# -- Compiler auto-detect ------------------------------------------------------
CXX      ?= g++
CXXSTD   ?= -std=c++2b
WARNINGS := -Wall -Wextra -Wpedantic -Werror
INCLUDES := -Iincludes
THRDLNK  := -pthread

# Windows links Winsock (SocketUtils.cpp uses socket/bind/select/...);
# POSIX kernels ship sockets in libc, so nothing extra is needed there.
ifeq ($(OS),Windows_NT)
SOCKLNK := -lws2_32
else
SOCKLNK :=
endif

# -- Compiled locations --------------------------------------------------------
BUILD       := build
SRC_OBJDIR  := $(BUILD)/obj
TST_OBJDIR  := $(BUILD)/tests
EXE         :=

# On Windows, cl.exe needs the extension.
ifeq ($(OS),Windows_NT)
	ifneq ($(filter g++ clang++,$(CXX)),)
		# g++ / clang++ on Windows (e.g. MSYS2 / Git Bash): keep POSIX flow, .exe.
		EXE := .exe
		# cmd.exe's mkdir rejects forward-slash paths ("The syntax of the
		# command is incorrect"), so route directory creation through
		# PowerShell, which normalizes build/obj -> build\obj itself.
		MKDIR := powershell -NoProfile -Command New-Item -ItemType Directory -Force -Path
	else
		# Assume MSVC cl.exe; different flags + no .exe:
		CXX      := cl
		CXXSTD   := /std:c++latest /EHsc
		WARNINGS := /W4 /WX
		INCLUDES := /Iincludes
		MKDIR    := mkdir
		THRDLNK  :=
		EXE      :=
	endif
endif

# Default POSIX-safe directory creation unless overridden above.
MKDIR ?= mkdir -p

# -- Sources -------------------------------------------------------------------
SRC_FILES := $(wildcard src/*.cpp)
SRC_OBJ   := $(patsubst src/%.cpp,$(SRC_OBJDIR)/%.o,$(SRC_FILES))
# test_runner needs the actual implementation objects (SqlParser, BTree,
# Pager, EngineDispatcher, ...) for anything not header-only — but not
# src/main.cpp's main(), which would collide with tests/main.cpp's main().
SRC_OBJ_NO_MAIN := $(filter-out $(SRC_OBJDIR)/main.o,$(SRC_OBJ))
TST_FILES := $(wildcard tests/*.test.cpp) tests/main.cpp
TST_OBJ   := $(patsubst tests/%.cpp,$(TST_OBJDIR)/%.o,$(TST_FILES))

# -- Phony ---------------------------------------------------------------------
.PHONY: all run test smoke clean

all: $(BUILD)/opendb$(EXE) $(BUILD)/test_runner$(EXE)

# -- Directory bootstrap (created up-front) -----------------------------------
$(SRC_OBJDIR) $(TST_OBJDIR):
	$(MKDIR) $@

# -- Object compile rules ------------------------------------------------------
$(SRC_OBJDIR)/%.o: src/%.cpp | $(SRC_OBJDIR)
	$(CXX) $(CXXSTD) $(WARNINGS) $(INCLUDES) -c $< -o $@

$(TST_OBJDIR)/%.o: tests/%.cpp tests/test_framework.hpp | $(TST_OBJDIR)
	$(CXX) $(CXXSTD) $(WARNINGS) $(INCLUDES) -c $< -o $@

# -- Linking -------------------------------------------------------------------
$(BUILD)/opendb$(EXE): $(SRC_OBJ) include-deps | $(BUILD)
ifneq ($(CXX),g++)
ifneq ($(CXX),clang++)
	$(CXX) /Fe:$@ $(WARNINGS) $(SRC_OBJ)
else
	$(CXX) $(WARNINGS) $(SRC_OBJ) -o $@ $(THRDLNK) $(SOCKLNK)
endif
else
	$(CXX) $(WARNINGS) $(SRC_OBJ) -o $@ $(THRDLNK) $(SOCKLNK)
endif

$(BUILD)/test_runner$(EXE): $(TST_OBJ) $(SRC_OBJ_NO_MAIN) | $(BUILD)
ifeq ($(CXX),g++)
	$(CXX) $(WARNINGS) $(TST_OBJ) $(SRC_OBJ_NO_MAIN) -o $@ $(THRDLNK) $(SOCKLNK)
else ifeq ($(CXX),clang++)
	$(CXX) $(WARNINGS) $(TST_OBJ) $(SRC_OBJ_NO_MAIN) -o $@ $(THRDLNK) $(SOCKLNK)
else
	$(CXX) /Fe:$@ $(WARNINGS) $(TST_OBJ) $(SRC_OBJ_NO_MAIN)
endif

# -- Run / test / smoke --------------------------------------------------------
run: $(BUILD)/opendb$(EXE)
	./$(BUILD)/opendb$(EXE)

test: $(BUILD)/test_runner$(EXE)
	./$(BUILD)/test_runner$(EXE)

# Milestone-5 smoke test (spec §7): INSERT -> SELECT round trip in the REPL.
# (Pure PowerShell via build.ps1 also works on Windows where bash / mingw
# recipes mis-shell through cmd.exe per spec §6.3.)
smoke: $(BUILD)/opendb$(EXE)
	@echo "INSERT users {key:1,name:nikhil,age:30}" > build/smoke_input.txt
	@echo "SELECT users"                              >> build/smoke_input.txt
	@echo "EXIT"                                      >> build/smoke_input.txt
	-./$(BUILD)/opendb$(EXE) < build/smoke_input.txt
	@rm build/smoke_input.txt

# -- Convenience include-dependency marker so source files track headers -----
include-deps:
	@true

# -- Clean ---------------------------------------------------------------------
clean:
	@if exist $(BUILD) rmdir /S /Q $(BUILD) 2>nul & rem Windows cmd fallback
	@rm -rf $(BUILD) 2>/dev/null || true     # POSIX fallback

# -- Ignore header file pattern triggers (force a compile when needed) --------
$(SRC_OBJ): | include-deps
