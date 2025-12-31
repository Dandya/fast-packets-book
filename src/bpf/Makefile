LIBBPF=../../contrib/xdp-tools/lib/libbpf/src/libbpf.a
LIBBPF_INCLUDE=../../contrib/xdp-tools/lib/libbpf/include

RTE_SDK=~/dpdk
RTE_SDK_BUILD=~/dpdk/build
RTE_TARGET=x86_64-native-linux-gcc

PKGCONF ?= pkg-config
PC_FILE := $(shell $(PKGCONF) --path libdpdk 2>/dev/null)
CFLAGS += -O2 $(shell $(PKGCONF) --cflags libdpdk)
LDFLAGS_SHARED = $(shell $(PKGCONF) --libs libdpdk)
LDFLAGS_STATIC = $(shell $(PKGCONF) --static --libs libdpdk)

all: bpf_test filter_ebpf.elf ./fcc/filter_fcc.so

bpf_test: tests.c test_fcc.c test_native.c test_kernel.c test_libpcap.c test_dpdk.c read_ebpf.c ${LIBBPF}
	gcc -g $(CFLAGS) $^ -o $@ -lpcap -I${LIBBPF_INCLUDE} -lelf -lz $(LDFLAGS) $(LDFLAGS_STATIC)

filter_ebpf.elf: filter_ebpf.c
	env C_INCLUDE_PATH=/usr/include/x86_64-linux-gnu clang -g -O2 -target bpf $< -c -o $@

./fcc/filter_fcc.so: ./fcc/fcc
	make -C ./fcc create NAME=filter_fcc FILTER="(ip or ip6) and ( (tcp and ( (dst port 80 and (tcp[tcpflags] & tcp-syn) != 0) or (dst port 443 and (tcp[tcpflags] & (tcp-syn|tcp-ack)) == (tcp-syn|tcp-ack)) or (dst port 22 and (tcp[tcpflags] & tcp-syn) != 0) or (dst portrange 10000-20000 and not src port 53) )) or (udp and ( (dst port 53 and length > 100) or (dst port 123 and ip[8] == 0x48) or (src port 67 and dst port 68 and ether[0] & 1 == 0) or (dst port 5060 and udp[20:2] != 0x5349) or (dst port 1900 and ip[9] == 0x11 and ip[8] == 0x40) )))"

./fcc/fcc:
	make -C ./fcc

${LIBBPF}:
	make -C ../../contrib/xdp-tools/lib/libbpf/src

clean:
	rm ./bpf_test || true
	rm *.elf || true
	make -C ./fcc clean
	make -C ./kernel clean