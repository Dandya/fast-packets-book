all: bpf_test

bpf_test: test.c filter.c
	gcc -g $^ -o $@ -lpcap

clean:
	rm ./bpf_test || true