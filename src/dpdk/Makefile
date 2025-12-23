
RTE_SDK=~/dpdk
RTE_SDK_BUILD=~/dpdk/build
RTE_TARGET=x86_64-native-linux-gcc

PKGCONF ?= pkg-config
PC_FILE := $(shell $(PKGCONF) --path libdpdk 2>/dev/null)
CFLAGS += -O2 $(shell $(PKGCONF) --cflags libdpdk)
LDFLAGS_SHARED = $(shell $(PKGCONF) --libs libdpdk)
LDFLAGS_STATIC = $(shell $(PKGCONF) --static --libs libdpdk)

all: test_dpdk

test_dpdk:  test.c
		$(CC) $(CFLAGS) test.c -o $@ $(LDFLAGS) $(LDFLAGS_STATIC)

clean:
	rm test_dpdk || true