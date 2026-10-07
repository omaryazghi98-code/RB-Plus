# Stremio Plus download writer. SPDX-License-Identifier: GPL-3.0-or-later
ifdef PS5_PAYLOAD_SDK
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
include /opt/ps5-payload-sdk/toolchain/prospero.mk
endif

OUTPUT ?= ../../../build/download-writer/download-writer.elf
PATH_NAMES := -ffile-prefix-map=$(abspath $(CURDIR)/../../..)/=stremio-plus/
ifdef PS5_PAYLOAD_SDK
PATH_NAMES += -ffile-prefix-map=$(PS5_PAYLOAD_SDK)/=sdk/ -ffile-prefix-map=$(realpath $(PS5_PAYLOAD_SDK))/=sdk/
endif
CXXFLAGS := -std=c++20 -O2 -g -gno-record-command-line -Wall -Wextra -Werror \
	-fno-exceptions -fno-rtti $(PATH_NAMES)
LDFLAGS := -Wl,--build-id=none

.PHONY: all clean
all: $(OUTPUT)

$(OUTPUT): main.cpp ../protocol.hpp ../posix_at.hpp ../../../third_party/json.hpp Makefile
	@mkdir -p -- $(@D)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -f -- $(OUTPUT)
