# Project Name
TARGET = ceres

# Runs from RAM, stored in the QSPI flash by the Daisy bootloader (480 KB instead of 128 KB).
# One-time setup: ./flash.sh --install
APP_TYPE = BOOT_SRAM

# Compiler options
OPT=-Os

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
