
#include <errno.h>
#include <getopt.h>
#include <pcap.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// TODO Переделать filter.c
// TODO Конвертацию socket-filter в jit и запуск в userspace и в ядре
// TODO Загрузку eBPF, и запуск в jit и без в userspace и в ядре

// Включение тестирование фильтра cBPF в интерпритаторе libpcap,
#define TEST_CBPF_LIBPCAP 1
// Включение тестирования фильтра выполненного в виде отдельной функции.
#define TEST_FUNCTION 1

#if TEST_CBPF_LIBPCAP
// Включение оптимизации компиляции фильтра в cBPF.
#define OPTIMIZE_BPF 1
// Фильтр в формате строки.
#define FILTER "(ip or ip6) and ( \
		(tcp and ( \
			(dst port 80 and (tcp[tcpflags] & tcp-syn) != 0) or \
			(dst port 443 and (tcp[tcpflags] & (tcp-syn|tcp-ack)) == (tcp-syn|tcp-ack)) or \
			(dst port 22 and (tcp[tcpflags] & tcp-syn) != 0) or \
			(dst portrange 10000-20000 and not src port 53) \
		)) or \
		(udp and ( \
			(dst port 53 and length > 100) or \
			(dst port 123 and ip[8] == 0x48) or \
			(src port 67 and dst port 68 and ether[0] & 1 == 0) or \
			(dst port 5060 and udp[20:2] != 0x5349) or \
			(dst port 1900 and ip[9] == 0x01 and ip[8] == 0x40) \
		)) \
)"
#endif

#if TEST_FUNCTION
extern bool check_filter(const uint8_t* packet, size_t length);
#endif

// Структура статистики работы фильтра.
struct filter_stats {
	uint64_t filtered_packets;
	uint64_t true_filtered;
	uint64_t start_usec;
	uint64_t end_usec;
};

// Структура списка пакетов для фильтрации.
struct pkt_desc {
	uint32_t len;
	uint32_t caplen;
	u_char data[];
};

// Выделение памяти для списка пакетов.
struct pkt_desc*
malloc_descs_list(size_t size) {
	return malloc(size);
}

// Освобождение памяти для списков пакетов.
void
free_descs_list(struct pkt_desc* first) {
	if (!first)
		return;
	free(first);
}

// Создание списка пакетов.
void
create_descs_list(const char* filename, struct pkt_desc** descs, size_t* count) {
	size_t size = 0;
	char errbuf[PCAP_ERRBUF_SIZE];
	pcap_t* handle = NULL;

	*descs = NULL;
	*count = 0;
	
	struct pcap_pkthdr hdr;
	const u_char* data = NULL;
	handle = pcap_open_offline(filename, errbuf);
	if (handle == NULL) {
		fprintf(stderr, "Open error %s: %s\n", filename, errbuf);
		return;
	}
	while ((pkt = pcap_next(handle, &hdr)) != NULL) {
		size += sizeof(struct pkt_desc) + hdr.caplen;
		*count += 1;
	}
	pcap_close(handle);

	*descs = malloc_descs_list(size);
	if (!*descs) {
		fprintf(stderr, "Error create packet list: %s\n", strerr(errno));
		return;
	}
	struct pkt_desc* pkt = *descs;
	handle = pcap_open_offline(filename, errbuf);
	if (handle == NULL) {
		free(*descs);
		*descs = NULL;
		fprintf(stderr, "Open error %s: %s\n", filename, errbuf);
		return;
	}
	while ((data = pcap_next(handle, &hdr)) != NULL) {
		pkt->len = hdr.len;
		pkt->caplen = hdr.caplen;
		memcpy(pkt->data, data, hdr.caplen);
		pkt = pkt + sizeof(struct pkt_desc) + pkt->caplen;
	}
	pcap_close(handle);
}

// Вспомогательная функция получения времени в наносекундах.
uint64_t
get_usec() {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((uint64_t)ts.tv_sec) * 1000000LL + ts.tv_nsec / 1000;
}

// Вспомогательная функция вывода статистики теста фильтра.
void
print_stat(const struct filter_stats* stat, const char* name) {
	uint64_t ts_usec = stas->end_usec - stas->start_usec;
	printf("=============\n%s\n", name)
	printf("Time (usecs): %llu\n", ts_usec);
	printf("Filtered (all): %llu\n", stat->filtered_packets);
	printf("Filtered (true): %llu\n", stat->true_filtered);
	printf("Speed in usec: %lf\n", ((double)(stat->filtered_packets)) / ts_usec);
}

#if TEST_CBPF_LIBPCAP 
void
test_cbpf_libpcap(struct pkt_desc* pkt, size_t count) {
	pcap_t *handle;
	char errbuf[PCAP_ERRBUF_SIZE];
	struct bpf_program bpf;
	struct filter_stats stat = {0, 0, 0 ,0};

	handle = pcap_open_dead(DLT_EN10, errbuf);
	if (handle == NULL) {
		fprintf(stderr, "Open error pcap: %s\n", errbuf);
		return;
	}

	if (pcap_compile(handle, &bpf, FILTER, OPTIMIZE_BPF, DLT_EN10) == -1) {
		fprintf(stderr, "Unsupported filter: %s\n", pcap_geterr(handle));
		pcap_close(handle);
		return;
	}

	pcap_close(handle);

	stat.start = get_usec();
	for (size_t i = 0; i < count; ++i) {
		stat.filtered_packets++;

		if (pcap_offline_filter_with_aux(bpf.insn, pkt->data, pkt->len, pkt->caplen, NULL))
			stat.true_filtered++;

		pkt = pkt + sizeof(struct pkt_desc) + pkt->caplen;
	}
	end = get_usec();
	pcap_freecode(&bpf);

	PrintStat(&stat, "cBPF-virt-libpcap");
}
#endif

#if TEST_FUNCTION
void
test_function(struct pkt_desc* pkt, size_t count) {
	struct filter_stats stat = {0, 0, 0 ,0};

	stat.start = GetUSec();
	for (size_t i = 0; i < count; ++i) {
		stat.filtered_packets++;

		if (check_filter(pkt->data, pkt->caplen))
			stat.true_filtered++;

		pkt = pkt + sizeof(struct pkt_desc) + pkt->caplen;
	}
	end = GetUSec();
	pcap_freecode(&bpf);

	PrintStat(&stat, "native-none-none");
}
#endif

int
main(int argc, char *argv[]) {
	const char *filename;

	if (argc < 2)
		return 1;

	filename = argv[1];

	size_t count = 0;
	struct pkt_desc* pkt = NULL;
	create_descs_list(filename, &pkt, &count);
	if (!pkt || !count) {
		fprintf(stderr, "Error of create packet list\n");
		return 1;
	}

#if TEST_CBPF_LIBPCAP
	test_cbpf_libpcap(pkt, count);
#endif
#if TEST_FUNCTION
	test_function(pkt, count);
#endif

	return 0;
}