LIBXDP_INCLUDE = ../../contrib/xdp-tools/headers
LIBXDP = ../../contrib/xdp-tools/lib/libxdp/libxdp.a
TARGETS = xdp_kern.o af_xdp_user
all: ${LIBXDP} ${TARGETS}

af_xdp_user: user.c
	gcc $< -o $@ -I${LIBXDP_INCLUDE} ${LIBXDP} -lbpf

xdp_kern.o: kern.c
	env C_INCLUDE_PATH=/usr/include/aarch64-linux-gnu clang -g -O2 -target bpf $< -c -o $@

${LIBXDP}:
	make -C ../../contrib/xdp-tools libxdp

clean:
	rm ${TARGETS} || true