
SRC_DIR    := src
OBJ_DIR    := obj

# --- third-party --------------------------------------------------------
THIRD_PARTY := $(abspath $(CURDIR)/third_party)
TP_CACHE    := $(THIRD_PARTY)/cache

# --- cosmocc toolchain --------------------------------------------------
COSMOCC_VER := 4.0.2
COSMOCC_ZIP := cosmocc-$(COSMOCC_VER).zip
COSMOCC_URL := https://cosmo.zip/pub/cosmocc/$(COSMOCC_ZIP)
COSMOCC_SHA := 85b8c37a406d862e656ad4ec14be9f6ce474c1b436b9615e91a55208aced3f44
COSMOCC_DIR := $(THIRD_PARTY)/cosmocc
COSMOCC_BIN := $(COSMOCC_DIR)/bin
COSMOCC_STAMP := $(COSMOCC_BIN)/x86_64-unknown-cosmo-c++

CC         := $(COSMOCC_BIN)/cosmocc
CXX        := $(COSMOCC_BIN)/cosmoc++

CXXFLAGS   := -std=c++20 -O2 -Wall -Wextra -fno-exceptions -fno-rtti \
              -isystem /usr/include -I$(OBJ_DIR)
LDFLAGS    := -pthread

WAYLAND_SCANNER := wayland-scanner
XDG_SHELL_XML   := /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml
CFLAGS     := -O2 -Wall -Wextra -isystem /usr/include -I$(OBJ_DIR)

TARGET     := apv

APP_OBJS   := $(OBJ_DIR)/main.o $(OBJ_DIR)/vk_dispatch.o $(OBJ_DIR)/xcb_api.o \
              $(OBJ_DIR)/window_session_xcb.o \
              $(OBJ_DIR)/wayland_api.o $(OBJ_DIR)/window_session_wayland.o \
              $(OBJ_DIR)/xdg-shell-protocol.o \
              $(OBJ_DIR)/window_session_win32.o

.PHONY: all clean clean-deps run ldd file test test-xcb test-wayland test-wine

all: $(TARGET) $(TARGET).exe

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

# ---- cosmocc toolchain --------------------------------------------------

$(TP_CACHE)/$(COSMOCC_ZIP):
	mkdir -p $(TP_CACHE)
	curl -fsSL -o $@.tmp $(COSMOCC_URL)
	echo "$(COSMOCC_SHA)  $@.tmp" | sha256sum -c -
	mv $@.tmp $@

$(COSMOCC_STAMP): $(TP_CACHE)/$(COSMOCC_ZIP)
	mkdir -p $(COSMOCC_DIR)
	cd $(COSMOCC_DIR) && unzip -qo $<
	touch $@

# ---- app objects -------------------------------------------------------

$(OBJ_DIR)/main.o: $(SRC_DIR)/main.cpp $(SRC_DIR)/vk_dispatch.hpp $(SRC_DIR)/window_session.hpp $(SRC_DIR)/window_session_xcb.hpp $(SRC_DIR)/any_subclass.hpp | $(OBJ_DIR) $(COSMOCC_STAMP)
	$(CXX) $(CXXFLAGS) -c $(SRC_DIR)/main.cpp -o $@

$(OBJ_DIR)/vk_dispatch.o: $(SRC_DIR)/vk_dispatch.cpp $(SRC_DIR)/vk_dispatch.hpp | $(OBJ_DIR) $(COSMOCC_STAMP)
	$(CXX) $(CXXFLAGS) -c $(SRC_DIR)/vk_dispatch.cpp -o $@

$(OBJ_DIR)/xcb_api.o: $(SRC_DIR)/xcb_api.cpp $(SRC_DIR)/xcb_api.hpp | $(OBJ_DIR) $(COSMOCC_STAMP)
	$(CXX) $(CXXFLAGS) -c $(SRC_DIR)/xcb_api.cpp -o $@

$(OBJ_DIR)/window_session_xcb.o: $(SRC_DIR)/window_session_xcb.cpp $(SRC_DIR)/window_session_xcb.hpp $(SRC_DIR)/window_session.hpp $(SRC_DIR)/xcb_api.hpp $(SRC_DIR)/vk_dispatch.hpp | $(OBJ_DIR) $(COSMOCC_STAMP)
	$(CXX) $(CXXFLAGS) -c $(SRC_DIR)/window_session_xcb.cpp -o $@

$(OBJ_DIR)/wayland_api.o: $(SRC_DIR)/wayland_api.cpp $(SRC_DIR)/wayland_api.hpp | $(OBJ_DIR) $(COSMOCC_STAMP)
	$(CXX) $(CXXFLAGS) -c $(SRC_DIR)/wayland_api.cpp -o $@

$(OBJ_DIR)/window_session_wayland.o: $(SRC_DIR)/window_session_wayland.cpp $(SRC_DIR)/window_session_wayland.hpp $(SRC_DIR)/window_session.hpp $(SRC_DIR)/wayland_api.hpp $(SRC_DIR)/vk_dispatch.hpp $(OBJ_DIR)/xdg-shell-client-protocol.h | $(OBJ_DIR) $(COSMOCC_STAMP)
	$(CXX) $(CXXFLAGS) -c $(SRC_DIR)/window_session_wayland.cpp -o $@

$(OBJ_DIR)/window_session_win32.o: $(SRC_DIR)/window_session_win32.cpp $(SRC_DIR)/window_session_win32.hpp $(SRC_DIR)/window_session.hpp $(SRC_DIR)/vk_dispatch.hpp | $(OBJ_DIR) $(COSMOCC_STAMP)
	$(CXX) $(CXXFLAGS) -c $(SRC_DIR)/window_session_win32.cpp -o $@

# --- wayland-scanner codegen -------------------------------------------

$(OBJ_DIR)/xdg-shell-client-protocol.h: $(XDG_SHELL_XML) | $(OBJ_DIR)
	$(WAYLAND_SCANNER) client-header $< $@

$(OBJ_DIR)/xdg-shell-protocol.c: $(XDG_SHELL_XML) | $(OBJ_DIR)
	$(WAYLAND_SCANNER) private-code $< $@

$(OBJ_DIR)/xdg-shell-protocol.o: $(OBJ_DIR)/xdg-shell-protocol.c | $(OBJ_DIR) $(COSMOCC_STAMP)
	$(CC) $(CFLAGS) -c $< -o $@

# Post-link step patches the APE's embedded PE header stack reserve.
# Cosmocc ships a 64 KiB default that NVIDIA's Vulkan ICD overruns.
# See scripts/patch_pe_stack.py.
PE_STACK_RESERVE := 8388608   # 8 MiB
PE_STACK_COMMIT  := 65536     # 64 KiB

$(TARGET): $(APP_OBJS) scripts/patch_pe_stack.py | $(COSMOCC_STAMP)
	$(CXX) $(LDFLAGS) $(APP_OBJS) -o $@
	python3 scripts/patch_pe_stack.py $@ $(PE_STACK_RESERVE) $(PE_STACK_COMMIT)

# PowerShell/cmd won't run an extensionless binary.
$(TARGET).exe: $(TARGET)
	cp $< $@

run: $(TARGET)
	DISPLAY=$${DISPLAY:-:0} XAUTHORITY=$${XAUTHORITY:-$$HOME/.Xauthority} ./$(TARGET)

ldd: $(TARGET)
	-ldd ./$(TARGET)

file: $(TARGET)
	file ./$(TARGET)
	@echo "size:"; stat -c '%s bytes' ./$(TARGET)


TEST_TIMEOUT := 3
TEST_MARKER  := entering render loop

test: test-xcb test-wayland

test-xcb: $(TARGET)
	@if [ -z "$$DISPLAY" ]; then \
	  echo "test-xcb: DISPLAY unset — skipping"; exit 0; \
	fi
	@echo "test-xcb: running ./$(TARGET) on DISPLAY=$$DISPLAY (${TEST_TIMEOUT}s)"
	@out=$$(APV_BACKEND=xcb timeout $(TEST_TIMEOUT) ./$(TARGET) 2>&1); \
	echo "$$out"; \
	echo "$$out" | grep -q "$(TEST_MARKER)" \
	  && echo "test-xcb: PASS" \
	  || { echo "test-xcb: FAIL (marker '$(TEST_MARKER)' not seen)"; exit 1; }

test-wayland: $(TARGET)
	@if ! command -v weston >/dev/null 2>&1; then \
	  echo "test-wayland: weston not installed — skipping"; exit 0; \
	fi
	@if [ -z "$$DISPLAY" ]; then \
	  echo "test-wayland: DISPLAY unset (nested weston needs X) — skipping"; \
	  exit 0; \
	fi
	@echo "test-wayland: running ./$(TARGET) under nested weston (${TEST_TIMEOUT}s)"
	@out=$$(weston --backend=x11 --no-config --socket=apv-test \
	        --width=800 --height=600 \
	        -- timeout $(TEST_TIMEOUT) $(CURDIR)/$(TARGET) 2>&1); \
	echo "$$out"; \
	echo "$$out" | grep -q "$(TEST_MARKER)" \
	  && echo "test-wayland: PASS" \
	  || { echo "test-wayland: FAIL (marker '$(TEST_MARKER)' not seen)"; exit 1; }

clean:
	rm -rf $(OBJ_DIR) $(TARGET) $(TARGET).exe $(TARGET).aarch64.elf $(TARGET).com.dbg

# Nukes fetched cosmocc toolchain.
clean-deps:
	rm -rf $(THIRD_PARTY)
