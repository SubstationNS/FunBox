# Project Name
TARGET = ceres

#APP_TYPE = BOOT_SRAM

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
