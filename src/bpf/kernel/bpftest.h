
#pragma once

#include <linux/errname.h>
#include <linux/if.h>
#include <linux/filter.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/version.h>

struct bpftest_shared_mem {
	struct page** pages;
	size_t pages_count;
	void* addr;
};

enum bpftest_bpf_type {
	BPFTEST_FILTER = 0,
	BPFTEST_PROGRAM = 1
};

struct bpftest_bpf {
	enum bpftest_bpf_type type;
	void* ctx;
	void* prog;
};

struct bpftest_pkt_desc {
	u32 len;
	u32 caplen;
	u64 offset_to_next;
	u8 data[];
};


#define PRINT_ERROR(str) \
	pr_err("bpftest: (%s:%d) %s\n", __func__, __LINE__, str)

// bpftest_bpf.c
int bpftest_remove_bpf(struct bpftest_bpf* bpf);
int bpftest_create_bpf(struct bpftest_bpf* bpf, void* insns, size_t len, bool jit_enable);
u8 bpftest_run_bpf(struct bpftest_bpf* bpf, struct bpftest_pkt_desc* desc);
u8 bpftest_is_jitted_bpf(struct bpftest_bpf* bpf);

// bpftest_ioctl.c
int bpftest_init_ioctl(void);
void bpftest_deinit_ioctl(void);

// bpftest_mem.c
int bpftest_map_mem(void __user* addr, unsigned long size, struct bpftest_shared_mem* desc);
void bpftest_unmap_mem(struct bpftest_shared_mem* desc);