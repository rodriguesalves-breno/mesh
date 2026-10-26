# Mesh - incremental, dependency tracked build.
#
# BLAKE2b is compiled into the binary (src/blake2b.c), so a machine only
# needs libcurl and pthreads to build. No external hash library is required
# or probed.

CC             ?= cc
PYTHON         ?= python3
PYTHONFLAGS    ?= -B
CFLAGS         ?= -std=c17 -O3 -Wall -Wextra
MESH_CURL_CFLAGS = $(shell pkg-config --cflags libcurl 2>/dev/null)
MESH_CURL_LIBS   = $(shell pkg-config --libs libcurl 2>/dev/null)
MESH_ICU_CFLAGS  = $(shell pkg-config --cflags icu-uc 2>/dev/null)
MESH_ICU_LIBS    = $(shell pkg-config --libs icu-uc 2>/dev/null)
CPPFLAGS       += -Isrc -D_FILE_OFFSET_BITS=64 $(MESH_CURL_CFLAGS) $(MESH_ICU_CFLAGS)
LDFLAGS        ?=
LDLIBS         ?= $(if $(MESH_CURL_LIBS),$(MESH_CURL_LIBS),-lcurl) $(if $(MESH_ICU_LIBS),$(MESH_ICU_LIBS),-licuuc -licudata) -pthread

BIN  := mesh
SRC  := src/main.c src/cli.c src/env.c src/network.c src/blake2b.c \
	src/chunking.c src/graph.c src/conflict.c src/monitor.c src/bridge.c \
	src/unicode.c src/git_coexist.c \
	src/ignore.c src/branch.c src/tag.c src/reflog.c src/clean.c \
	src/inflate.c src/archive.c src/obtain.c src/admin.c \
	src/admin_support.c src/admin_doctor.c src/admin_verify.c \
	src/admin_inventory.c src/admin_gc.c src/admin_refs.c \
	src/admin_status.c src/admin_audit.c src/admin_quota.c \
	src/admin_config.c src/admin_history.c src/admin_export.c
OBJ  := $(SRC:.c=.o)
CORE_OBJ := $(filter-out src/main.o,$(OBJ))
DEP  := $(OBJ:.o=.d)

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.c src/core.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -MMD -MP -c -o $@ $<

# Publish the self-contained Python installer. It verifies the toolchain,
# installs the binary and wires PATH for the current user.
mesh-install: install_uninstall.py
	cp install_uninstall.py $@
	chmod 0755 $@

install: $(BIN) mesh-install
	./mesh-install

# Remove the installed binary, icons and PATH blocks; keep global settings.
uninstall: mesh-install
	./mesh-install --uninstall

# Remove the installed files and the user's global Mesh configuration.
purge: mesh-install
	./mesh-install --uninstall --purge

check: $(BIN)
	./$(BIN) --version

tools-check:
	$(PYTHON) $(PYTHONFLAGS) tools/module_check.py
	$(PYTHON) $(PYTHONFLAGS) tools/module_docs.py --check
	$(PYTHON) $(PYTHONFLAGS) -m py_compile tools/unicode_audit.py tools/git_import.py tests/test_unicode_audit.py tests/test_git_import.py tools/history_restart.py tests/test_history_restart.py tools/git_coexist.py tests/test_git_coexist.py

tools-test: $(BIN)
	BIN=$(CURDIR)/$(BIN) $(PYTHON) $(PYTHONFLAGS) tools/module_test.py

tools-docs:
	$(PYTHON) $(PYTHONFLAGS) tools/module_docs.py --write

tests/unit: tests/unit.c $(CORE_OBJ) src/core.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ tests/unit.c $(CORE_OBJ) \
		$(LDLIBS)

unit: tests/unit
	./tests/unit
	$(PYTHON) $(PYTHONFLAGS) tests/test_unicode_audit.py

test: $(BIN) mesh-install tests/unit
	./tests/unit
	$(PYTHON) $(PYTHONFLAGS) tests/test_unicode_audit.py
	BIN=$(CURDIR)/$(BIN) $(PYTHON) $(PYTHONFLAGS) tests/test_mesh.py
	BIN=$(CURDIR)/$(BIN) $(PYTHON) $(PYTHONFLAGS) tests/test_sync.py
	BIN=$(CURDIR)/$(BIN) $(PYTHON) $(PYTHONFLAGS) tests/test_obtain.py
	BIN=$(CURDIR)/$(BIN) $(PYTHON) $(PYTHONFLAGS) tests/test_extras.py
	INSTALLER=$(CURDIR)/mesh-install $(PYTHON) $(PYTHONFLAGS) tests/test_installer.py
	MESH_BIN=$(CURDIR)/$(BIN) $(PYTHON) $(PYTHONFLAGS) tests/test_git_import.py
	MESH_BIN=$(CURDIR)/$(BIN) $(PYTHON) $(PYTHONFLAGS) tests/test_history_restart.py
	MESH_BIN=$(CURDIR)/$(BIN) $(PYTHON) $(PYTHONFLAGS) tests/test_git_coexist.py
	$(PYTHON) $(PYTHONFLAGS) tools/module_check.py
	$(PYTHON) $(PYTHONFLAGS) tools/module_docs.py --check
	$(PYTHON) $(PYTHONFLAGS) -m py_compile tools/unicode_audit.py tools/git_import.py tests/test_unicode_audit.py tests/test_git_import.py tools/history_restart.py tests/test_history_restart.py tools/git_coexist.py tests/test_git_coexist.py
	BIN=$(CURDIR)/$(BIN) $(PYTHON) $(PYTHONFLAGS) tools/module_test.py

sanitize:
	$(MAKE) clean
	$(MAKE) CC=clang CFLAGS='-std=c17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Werror' \
		LDFLAGS='-fsanitize=address,undefined' all mesh-install tests/unit
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./tests/unit
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
		BIN=$(CURDIR)/$(BIN) $(PYTHON) $(PYTHONFLAGS) tests/test_mesh.py
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
		BIN=$(CURDIR)/$(BIN) $(PYTHON) $(PYTHONFLAGS) tests/test_sync.py
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
		BIN=$(CURDIR)/$(BIN) $(PYTHON) $(PYTHONFLAGS) tests/test_obtain.py
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
		BIN=$(CURDIR)/$(BIN) $(PYTHON) $(PYTHONFLAGS) tests/test_extras.py
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
		INSTALLER=$(CURDIR)/mesh-install $(PYTHON) $(PYTHONFLAGS) tests/test_installer.py
	MESH_BIN=$(CURDIR)/$(BIN) $(PYTHON) $(PYTHONFLAGS) tests/test_git_import.py
	MESH_BIN=$(CURDIR)/$(BIN) $(PYTHON) $(PYTHONFLAGS) tests/test_history_restart.py
	MESH_BIN=$(CURDIR)/$(BIN) $(PYTHON) $(PYTHONFLAGS) tests/test_git_coexist.py
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
		BIN=$(CURDIR)/$(BIN) $(PYTHON) $(PYTHONFLAGS) tools/module_test.py

# The CI target is intentionally strict: no warnings, then the same tests.
ci:
	$(MAKE) clean
	$(MAKE) CFLAGS='-std=c17 -O2 -Wall -Wextra -Werror' all mesh-install tests/unit
	./tests/unit
	$(PYTHON) $(PYTHONFLAGS) tests/test_unicode_audit.py
	BIN=$(CURDIR)/$(BIN) $(PYTHON) $(PYTHONFLAGS) tests/test_mesh.py
	BIN=$(CURDIR)/$(BIN) $(PYTHON) $(PYTHONFLAGS) tests/test_sync.py
	BIN=$(CURDIR)/$(BIN) $(PYTHON) $(PYTHONFLAGS) tests/test_obtain.py
	BIN=$(CURDIR)/$(BIN) $(PYTHON) $(PYTHONFLAGS) tests/test_extras.py
	INSTALLER=$(CURDIR)/mesh-install $(PYTHON) $(PYTHONFLAGS) tests/test_installer.py
	MESH_BIN=$(CURDIR)/$(BIN) $(PYTHON) $(PYTHONFLAGS) tests/test_git_import.py
	MESH_BIN=$(CURDIR)/$(BIN) $(PYTHON) $(PYTHONFLAGS) tests/test_history_restart.py
	MESH_BIN=$(CURDIR)/$(BIN) $(PYTHON) $(PYTHONFLAGS) tests/test_git_coexist.py
	$(PYTHON) $(PYTHONFLAGS) tools/module_check.py
	$(PYTHON) $(PYTHONFLAGS) tools/module_docs.py --check
	$(PYTHON) $(PYTHONFLAGS) -m py_compile tools/unicode_audit.py tools/git_import.py tests/test_unicode_audit.py tests/test_git_import.py tools/history_restart.py tests/test_history_restart.py tools/git_coexist.py tests/test_git_coexist.py
	BIN=$(CURDIR)/$(BIN) $(PYTHON) $(PYTHONFLAGS) tools/module_test.py

# Static analysis is slow and GCC-specific; keep it out of the default build.
analyze:
	$(MAKE) clean
	$(MAKE) CFLAGS='-std=c17 -O2 -Wall -Wextra -Werror -fanalyzer' \
		all mesh-install

# Coverage-guided fuzzing of the manifest parser. Requires Clang with
# libFuzzer and the ASan/UBSan runtimes.
FUZZ_RUNS ?= 2000
FUZZ_MAX_LEN ?= 65536
fuzz:
	$(MAKE) clean
	$(MAKE) CC=clang CFLAGS='-std=c17 -O1 -g -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -Wall -Wextra -Werror' \
		tests/fuzz_manifest
	./tests/fuzz_manifest -runs=$(FUZZ_RUNS) -max_len=$(FUZZ_MAX_LEN)

tests/fuzz_manifest: tests/fuzz_manifest.c $(CORE_OBJ) src/core.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ tests/fuzz_manifest.c \
		$(CORE_OBJ) $(LDLIBS)

clean:
	rm -f $(OBJ) $(DEP) $(BIN) mesh-install tests/unit tests/fuzz_manifest
	rm -rf __pycache__ tests/__pycache__ tools/__pycache__

-include $(DEP)

.PHONY: all install uninstall purge check tools-check tools-test tools-docs unit test sanitize ci analyze fuzz clean
