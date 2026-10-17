# Project Name
TARGET = ceres

# Runs from RAM, stored in the QSPI flash by the Daisy bootloader (480 KB instead of 128 KB).
# One-time setup: ./flash.sh --install
APP_TYPE = BOOT_SRAM

# Compiler options
OPT=-O2  # Runs from RAM (480 KB): speed over size

# Sources
CPP_SOURCES = ceres.cpp

# Library Locations
LIBDAISY_DIR = ../../libDaisy
DAISYSP_DIR = ../../DaisySP

# Core location, and generic Makefile.
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile

# Include funbox.h
C_INCLUDES += -I../../include

# Firmware version for the log: the git commit (build dates are pinned to 1980 in nix builds).
# Written to build/ceres_version.h only when it changes, so ceres.cpp recompiles exactly then.
CERES_VERSION := $(shell git describe --always --dirty 2>/dev/null || echo unknown)
$(shell mkdir -p $(BUILD_DIR) && echo '#define CERES_VERSION "$(CERES_VERSION)"' > $(BUILD_DIR)/ceres_version.h.new \
    && (cmp -s $(BUILD_DIR)/ceres_version.h.new $(BUILD_DIR)/ceres_version.h || mv $(BUILD_DIR)/ceres_version.h.new $(BUILD_DIR)/ceres_version.h); \
    rm -f $(BUILD_DIR)/ceres_version.h.new)
C_INCLUDES += -I$(BUILD_DIR)
