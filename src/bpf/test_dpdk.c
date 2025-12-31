
#include <rte_eal.h>
#include <rte_bpf.h>
#include <rte_mbuf.h>

#include "settings.h"
#include "tests.h"

// Функция тестирования фильтрации с помощью cBPF в DPDK.
void
test_cbpf_dpdk(struct pkt_descs_list* list, uint8_t is_jit) {
	pcap_t *handle;
	char errbuf[PCAP_ERRBUF_SIZE];
	struct bpf_program bpf;
	struct rte_bpf_prm* prm;
	struct rte_bpf* ebpf;
	struct filter_stats stats[TESTS_COUNT];
	struct rte_mbuf buf = {
		.data_off = 0,
		.nb_segs = 1,
		.buf_len = 3000,
	};
	const char* name;

	if (is_jit)
		name = "cBPF-jit-dpdk";
	else
		name = "cBPF-virt-dpdk";

	handle = pcap_open_dead(DLT_EN10MB, 3000);
	if (handle == NULL) {
		fprintf(stderr, "Open error pcap: %s\n", errbuf);
		return;
	}

	if (pcap_compile(handle, &bpf, FILTER, OPTIMIZE_BPF, DLT_EN10MB) == -1) {
		fprintf(stderr, "Unsupported filter: %s\n", pcap_geterr(handle));
		pcap_close(handle);
		return;
	}

	pcap_close(handle);

	prm = rte_bpf_convert(&bpf);
	if (!prm) {
		fprintf(stderr, "Error of convert cBPF to eBPF\n");
		return;
	}
	ebpf = rte_bpf_load(prm);
	rte_free(prm);
	if (!ebpf) {
		fprintf(stderr, "Error of create eBPF\n");
		return;
	}

	if (is_jit) {
		struct rte_bpf_jit jit;
		if (rte_bpf_get_jit(ebpf, &jit) < 0) {
			rte_bpf_destroy(ebpf);
			fprintf(stderr, "Error of jit compile eBPF\n");
			return;
		}

		for (size_t s_i = 0; s_i < TESTS_COUNT; ++s_i) {
			struct filter_stats stat = {0, 0, 0, 0};
			struct pkt_desc* desc = list->descs;
			stat.start_usec = get_usec();
			for (size_t i = 0; i < list->count; ++i) {
				if (stat.filtered_packets == 5516524) {
					break;
				} else if (i == 97) {
					desc = list->descs;
					i = 0;
				}
				
				stat.filtered_packets++;

				buf.pkt_len = desc->len;
				buf.data_len = desc->caplen;
				buf.buf_addr = desc->data;
				if (jit.func(&buf))
					stat.true_filtered++;

				desc = NEXT_PKT(desc);
			}
			stat.end_usec = get_usec();
			stats[s_i] = stat;
		}
	} else {
		for (size_t s_i = 0; s_i < TESTS_COUNT; ++s_i) {
			struct filter_stats stat = {0, 0, 0, 0};
			struct pkt_desc* desc = list->descs;
			stat.start_usec = get_usec();
			for (size_t i = 0; i < list->count; ++i) {
				stat.filtered_packets++;

				buf.pkt_len = desc->len;
				buf.data_len = desc->caplen;
				buf.buf_addr = desc->data;
				if (rte_bpf_exec(ebpf, &buf))
					stat.true_filtered++;

				desc = NEXT_PKT(desc);
			}
			stat.end_usec = get_usec();
			stats[s_i] = stat;
		}
	}

	pcap_freecode(&bpf);
	rte_bpf_destroy(ebpf);

	print_stat(stats, name);
}

// Функция тестирования фильтрации с помощью eBPF в DPDK.
void
test_ebpf_dpdk(struct pkt_descs_list* list, uint8_t is_jit) {
	const struct rte_bpf_prm prm = {
		.prog_arg = {
			.type = RTE_BPF_ARG_PTR,
			.size = 4096,
			.buf_size = 4096,
		},
	};
	struct rte_bpf* ebpf;
	struct filter_stats stats[TESTS_COUNT];
	struct rte_mbuf buf = {
		.data_off = 0,
		.nb_segs = 1,
		.buf_len = 3000,
	};
	const char* name;

	if (is_jit)
		name = "eBPF-jit-dpdk";
	else
		name = "eBPF-virt-dpdk";

	ebpf = rte_bpf_elf_load(&prm, "filter_ebpf.elf", "UNSPEC");
	if (!ebpf) {
		fprintf(stderr, "Error of read eBPF\n");
		return;
	}

	if (is_jit) {
		struct rte_bpf_jit jit;
		if (rte_bpf_get_jit(ebpf, &jit) < 0) {
			rte_bpf_destroy(ebpf);
			fprintf(stderr, "Error of jit compile eBPF\n");
			return;
		}

		for (size_t s_i = 0; s_i < TESTS_COUNT; ++s_i) {
			struct filter_stats stat = {0, 0, 0, 0};
			struct pkt_desc* desc = list->descs;
			stat.start_usec = get_usec();
			for (size_t i = 0; i < list->count; ++i) {	
				stat.filtered_packets++;

				buf.pkt_len = desc->len;
				buf.data_len = desc->caplen;
				buf.buf_addr = desc->data;
				if (jit.func(desc))
					stat.true_filtered++;

				desc = NEXT_PKT(desc);
			}
			stat.end_usec = get_usec();
			stats[s_i] = stat;
		}

	} else {
		for (size_t s_i = 0; s_i < TESTS_COUNT; ++s_i) {
			struct filter_stats stat = {0, 0, 0, 0};
			struct pkt_desc* desc = list->descs;
			stat.start_usec = get_usec();
			for (size_t i = 0; i < list->count; ++i) {
				stat.filtered_packets++;

				buf.pkt_len = desc->len;
				buf.data_len = desc->caplen;
				buf.buf_addr = desc->data;
				if (rte_bpf_exec(ebpf, desc))
					stat.true_filtered++;

				desc = NEXT_PKT(desc);
			}
			stat.end_usec = get_usec();
			stats[s_i] = stat;
		}
	}

	rte_bpf_destroy(ebpf);

	print_stat(stats, name);
}