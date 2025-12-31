
#include <rte_eal.h>
#include <rte_bpf.h>
#include <rte_mbuf.h>

#include "settings.h"
#include "tests.h"

#if TEST_CBPF_LIBPCAP
extern void test_cbpf_libpcap(struct pkt_descs_list* list);
#endif

#if TEST_CBPF_FCC
extern void test_fcc(struct pkt_descs_list* list);
#endif

#if TEST_FUNCTION
extern void test_function(struct pkt_descs_list* list);
#endif

#if TEST_CBPF_VIRT_KERNEL || TEST_CBPF_JIT_KERNEL || TEST_EBPF_VIRT_KERNEL || TEST_EBPF_JIT_KERNEL
extern void test_cbpf_kernel(struct pkt_descs_list* list, uint8_t is_jit);
extern void test_ebpf_kernel(struct pkt_descs_list* list, uint8_t is_jit);
#endif

#if TEST_CBPF_VIRT_DPDK || TEST_CBPF_JIT_DPDK || TEST_EBPF_VIRT_DPDK || TEST_EBPF_JIT_DPDK
extern void test_cbpf_dpdk(struct pkt_descs_list* list, uint8_t is_jit);
extern void test_ebpf_dpdk(struct pkt_descs_list* list, uint8_t is_jit);
#endif


// Выделение памяти для списка пакетов.
void
malloc_descs_list(long page_size, size_t size, struct pkt_descs_list* list) {
	list->descs = aligned_alloc(page_size, size);
}

// Освобождение памяти для списков пакетов.
void
free_descs_list(struct pkt_descs_list* list) {
	if (!list)
		return;
	free(list->descs);
	list->descs = NULL;
}

// Создание списка пакетов.
void
create_descs_list(const char* filename, struct pkt_descs_list* list) {
	char errbuf[PCAP_ERRBUF_SIZE];
	pcap_t* handle = NULL;
	struct pcap_pkthdr hdr;
	const u_char* data = NULL;
	struct pkt_desc* desc = NULL;
	
	list->descs = NULL;
	list->count = 0;
	list->len = 0;
	
	handle = pcap_open_offline(filename, errbuf);
	if (handle == NULL) {
		fprintf(stderr, "Open error %s: %s\n", filename, errbuf);
		return;
	}
	while ((data = pcap_next(handle, &hdr)) != NULL) {
		size_t old_page_indx = list->len / PAGE_SIZE;
		list->len += sizeof(struct pkt_desc) + hdr.caplen;
		// Проверка допустимой длины для выравнивания.
		if (old_page_indx != (list->len / PAGE_SIZE)) 
			list->len += sizeof(struct pkt_desc) + hdr.caplen - (list->len % PAGE_SIZE); 
		list->count += 1;
	}
	pcap_close(handle);

	malloc_descs_list(PAGE_SIZE, list->len, list);
	if (!list->descs) {
		fprintf(stderr, "Error create packet list: %s\n", strerror(errno));
		return;
	}

	handle = pcap_open_offline(filename, errbuf);
	if (handle == NULL) {
		free_descs_list(list);
		fprintf(stderr, "Open error %s: %s\n", filename, errbuf);
		return;
	}
	desc = list->descs;
	struct pkt_desc* last_desc = NULL;
	while ((data = pcap_next(handle, &hdr)) != NULL) { 
		if (last_desc) {
			if (PAGE_ALIGN(NEXT_PKT(last_desc)) != PAGE_ALIGN((uint8_t*)NEXT_PKT(last_desc) +
					sizeof(struct pkt_desc) + hdr.caplen))
				last_desc->offset_to_next = PAGE_ALIGN((uint8_t*)NEXT_PKT(last_desc) +
						sizeof(struct pkt_desc) + hdr.caplen) - (uintptr_t)last_desc;
			desc = NEXT_PKT(last_desc);
		}
		desc->len = hdr.len;
		desc->caplen = hdr.caplen;
		desc->offset_to_next = sizeof(struct pkt_desc) + desc->caplen;
		memcpy(desc->data, data, hdr.caplen);
		last_desc = desc;
	}
	assert(((uintptr_t)desc + desc->caplen - (uintptr_t)list->descs <= list->len));
	pcap_close(handle);
}

int
main(int argc, char *argv[]) {
	const char *filename;
	struct pkt_descs_list list;

	int ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Error: EAL initialization failed\n");

	// Обработка параметров.
	argc -= ret;
	argv += ret;

	if (argc < 2)
		return 1;

	filename = argv[1];


	printf("Read packets...\n");
	create_descs_list(filename, &list);
	if (!list.descs || !list.count) {
		fprintf(stderr, "Error of create packet list\n");
		return -1;
	}
	printf("Read done\n");

#if TEST_CBPF_LIBPCAP
	test_cbpf_libpcap(&list);
#endif
#if TEST_CBPF_FCC
	test_fcc(&list);
#endif
#if TEST_FUNCTION
	test_function(&list);
#endif
#if TEST_CBPF_VIRT_KERNEL
	test_cbpf_kernel(&list, 0);
#endif
#if TEST_EBPF_VIRT_KERNEL
	test_ebpf_kernel(&list, 0);
#endif
#if TEST_CBPF_JIT_KERNEL
	test_cbpf_kernel(&list, 1);
#endif
#if TEST_EBPF_JIT_KERNEL
	test_ebpf_kernel(&list, 1);
#endif
#if TEST_CBPF_VIRT_DPDK
	test_cbpf_dpdk(&list, 0);
#endif
#if TEST_CBPF_JIT_DPDK
	test_cbpf_dpdk(&list, 1);
#endif
#if TEST_EBPF_VIRT_DPDK
	test_ebpf_dpdk(&list, 0);
#endif
#if TEST_EBPF_JIT_DPDK
	test_ebpf_dpdk(&list, 1);
#endif

	return 0;
}