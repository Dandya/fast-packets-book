
#include <linux/filter.h>

#include "bpftest.h"

#define BPFTEST_SKB_BUFF (9 * 1024)

static struct net_device* bpftest_create_tmp_device(void) {
	return alloc_netdev(0, "tmp_i%d", NET_NAME_ENUM, ether_setup);
}

static struct sk_buff *bpftest_create_skb(void) {
	static const char pkt[] = {
		0x08, 0x00, 0x27, 0x99, 0x66, 0xc5, 0x08, 0x00,
		0x27, 0xe5, 0xa9, 0x29, 0x08, 0x00, 0x45, 0x00,
		0x00, 0x3c, 0x32, 0x87, 0x40, 0x00, 0x3f, 0x06,
		0x86, 0xd8, 0xc0, 0xa8, 0x01, 0x02, 0xc0, 0xa8,
		0x00, 0x0a, 0x8c, 0x8c, 0x00, 0x50, 0xaa, 0xe1,
		0x0c, 0x62, 0x00, 0x00, 0x00, 0x00, 0xa0, 0x02,
		0xfa, 0xf0, 0x19, 0xb6, 0x00, 0x00, 0x02, 0x04,
		0x05, 0xb4, 0x04, 0x02, 0x08, 0x0a, 0xfa, 0xf8,
		0x71, 0xe3, 0x00, 0x00, 0x00, 0x00, 0x01, 0x03,
		0x03, 0x07
	}; // TCP-SYN 192.168.1.2 to 192.168.0.10
	struct sk_buff* skb = NULL;
	struct net_device* dev = NULL;

	dev = bpftest_create_tmp_device();
	if (!dev) return NULL;

	skb = alloc_skb(BPFTEST_SKB_BUFF + LL_RESERVED_SPACE(dev), GFP_KERNEL);
	if (!skb) {
		free_netdev(dev);
		return NULL;
	}

	skb_reserve(skb, LL_RESERVED_SPACE(dev));

	skb_put(skb, sizeof(pkt));
	memcpy(skb->data, pkt, sizeof(pkt));

	skb->mac_header = 0;
	skb->network_header = ETH_HLEN;

	skb->protocol = eth_type_trans(skb, dev);

	free_netdev(dev);

	return skb;
}

static int bpftest_create_filter(struct sock_filter* filter, size_t len,
		struct bpf_prog** bpf) {
	struct sock_fprog_kern fprog = {
		.len = len,
		.filter = filter,
	};
	int ret = 0;

	ret = bpf_prog_create(bpf, &fprog);
	if (ret < 0) {
		*bpf = NULL;
		return ret;
	}

	return 0;
}

static int bpftest_create_program(struct bpf_insn* insns, size_t len,
	struct bpf_prog** bpf, bool jit_enable) {
	int err = 0;
	struct bpf_prog* prog = NULL;

	prog = bpf_prog_alloc(bpf_prog_size(len), GFP_KERNEL);
	if (!prog) return -ENOMEM;

	prog->len = len;
	memcpy(prog->insns, insns, len * sizeof(struct bpf_insn));
	prog->jited = 0;
	prog->jit_requested = jit_enable;
	prog->gpl_compatible = 1; // ? Нужно ли
	prog->type = BPF_PROG_TYPE_UNSPEC;

	prog = bpf_prog_select_runtime(prog, &err);
	if (err != 0) {
		pr_debug("Error jit ebpf: %d\n", err);
		if (prog)
			bpf_prog_free(prog);
		return -EINVAL;
	}

	*bpf = prog;
	return 0;
}

int bpftest_remove_bpf(struct bpftest_bpf* bpf) {
	if (unlikely(!bpf))
		return -EINVAL;

	if (bpf->ctx) {
		if (bpf->type == BPFTEST_FILTER)
			kfree_skb(bpf->ctx);
		else
			BUG_ON(true);
		bpf->ctx = NULL;
	}

	if (bpf->prog) {
		if (bpf->type == BPFTEST_FILTER)
			bpf_prog_destroy(bpf->prog);
		else
			bpf_prog_free(bpf->prog);
		bpf->prog = NULL;
	}

	return 0;
}

int bpftest_create_bpf(struct bpftest_bpf* bpf, void* insns, size_t len, bool jit_enable) {
	if (unlikely(!bpf || !insns || !len))
		return -EINVAL;

	int ret = 0;

	if (bpf->type == BPFTEST_FILTER) {
		bpf->ctx = bpftest_create_skb();
		if (!bpf->ctx) return -ENOMEM;
	} else {
		bpf->ctx = NULL;
	}

	if (bpf->type == BPFTEST_FILTER)
		ret = bpftest_create_filter(insns, len, (struct bpf_prog**)&bpf->prog);
	else
		ret = bpftest_create_program(insns, len, (struct bpf_prog**)&bpf->prog, jit_enable);

	if (ret < 0)
		bpftest_remove_bpf(bpf);

	return ret;
}

u8 bpftest_run_bpf(struct bpftest_bpf* bpf, struct bpftest_pkt_desc* desc) {
	struct sk_buff skb;
	switch(bpf->type) {
		case BPFTEST_FILTER:
			skb = *(struct sk_buff*)bpf->ctx;
			skb.data = desc->data;
			skb.len = desc->caplen;
			return bpf_prog_run(bpf->prog, &skb);
		case BPFTEST_PROGRAM:
			return bpf_prog_run(bpf->prog, desc);
		default:
			return 0;
	}
}

u8 bpftest_is_jitted_bpf(struct bpftest_bpf* bpf) {
	return ((struct bpf_prog*)bpf->prog)->jited ? 1 : 0;
}

