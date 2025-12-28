
RTE_SDK=~/dpdk
RTE_SDK_BUILD=~/dpdk/build
RTE_TARGET=x86_64-native-linux-gcc

PKGCONF ?= pkg-config
PC_FILE := $(shell $(PKGCONF) --path libdpdk 2>/dev/null)
CFLAGS += -O2 $(shell $(PKGCONF) --cflags libdpdk)
LDFLAGS_SHARED = $(shell $(PKGCONF) --libs libdpdk)
LDFLAGS_STATIC = $(shell $(PKGCONF) --static --libs libdpdk)

all: dpdk_user

dpdk_user: user.c
		$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LDFLAGS_STATIC)

clean:
	rm dpdk_user || true