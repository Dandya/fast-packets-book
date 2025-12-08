LIBXDP_INCLUDE=../../contrib/xdp-tools/headers
LIBXDP=../../contrib/xdp-tools/lib/libxdp/libxdp.a

all: ${LIBXDP} xdp_kern.o af_xdp_user

af_xdp_user: user.c
	gcc $< -o $@ -I${LIBXDP_INCLUDE} ${LIBXDP} -lbpf

xdp_kern.o: kern.c
	 clang -g -O2 -target bpf $< -c -o $@

${LIBXDP}:
	make -C ../../contrib/xdp-tools libxdp

clean:
	rm xdp_kern.o || true
	rm af_xdp_user || true