# ps5-native-app-boilerplate - Sandbox elevation helper build.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later

ifdef PS5_PAYLOAD_SDK
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
include /opt/ps5-payload-sdk/toolchain/prospero.mk
endif

OUTPUT ?= ../../../build/elevation/sandbox-elevator.elf
TARGET_TITLE_ID ?= PPSA98273
# The debug information names the sources by short folder names, not by the folders of the
# machine that built it (this one and the SDK's, as given and as it resolves).
PATH_NAMES := -ffile-prefix-map=$(abspath $(CURDIR)/..)/=elevation/ \
	-ffile-prefix-map=$(abspath $(CURDIR)/..)/=filesystem/
ifdef PS5_PAYLOAD_SDK
PATH_NAMES += -ffile-prefix-map=$(PS5_PAYLOAD_SDK)/=sdk/ -ffile-prefix-map=$(realpath $(PS5_PAYLOAD_SDK))/=sdk/
endif
CXXFLAGS := -std=c++20 -O2 -g -gno-record-command-line -Wall -Wextra -Werror -DTARGET_TITLE_ID=\\\"$(TARGET_TITLE_ID)\\\" $(PATH_NAMES)
LDFLAGS := -Wl,--build-id=none

.PHONY: all clean

all: $(OUTPUT)

# This file is a prerequisite too: a change of flags builds the program again.
$(OUTPUT): main.cpp ../protocol.hpp Makefile
	@mkdir -p -- $(@D)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -f -- $(OUTPUT)
