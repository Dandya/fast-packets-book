
#include "settings.h"
#include "tests.h"

// Функция тестирования фильтрации с помощью cBPF в интерпритаторе libpcap.
void
test_cbpf_libpcap(struct pkt_descs_list* list) {
	pcap_t *handle;
	char errbuf[PCAP_ERRBUF_SIZE];
	struct bpf_program bpf;
	struct pcap_pkthdr hdr;
	struct filter_stats stats[TESTS_COUNT];

	handle = pcap_open_dead(DLT_EN10MB, 1500);
	if (handle == NULL) {
		fprintf(stderr, "Open error dead pcap\n");
		return;
	}

	if (pcap_compile(handle, &bpf, FILTER, OPTIMIZE_BPF, DLT_EN10MB) == -1) {
		fprintf(stderr, "Unsupported filter: %s\n", pcap_geterr(handle));
		pcap_close(handle);
		return;
	}

	pcap_close(handle);

	for (size_t s_i = 0; s_i < TESTS_COUNT; ++s_i) {
		struct filter_stats stat = {0, 0, 0, 0};
		struct pkt_desc* desc = list->descs;
		stat.start_usec = get_usec();
		for (size_t i = 0; i < list->count; ++i) {
			stat.filtered_packets++;

			hdr.len = desc->len;
			hdr.caplen = desc->caplen;
			if (pcap_offline_filter(&bpf, &hdr, desc->data))
				stat.true_filtered++;

			desc = NEXT_PKT(desc);
		}
		stat.end_usec = get_usec();
		stats[s_i] = stat;
	}
	pcap_freecode(&bpf);

	print_stat(stats, "cBPF-virt-libpcap");
}