
#include <errno.h>
#include <pcap.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// #include <arpa/inet.h>

// #include <netinet/if_ether.h>
// #include <netinet/ip.h>
// #include <netinet/tcp.h>
// #include <netinet/udp.h>
#define OPTIMIZE_BPF 1
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

extern bool check_filter(const uint8_t* packet, size_t length);

struct filter_stats {
	uint64_t filtered_packets;
	uint64_t true_filtered;
	uint64_t bytes_total;
};

struct pkt_desc {
	struct pcap_pkthdr hdr;
	u_char data[];
};

struct pkt_desc**
realloc_descs(struct pkt_desc** ptr, size_t* size) {
	static const size_t step = 1000000;

	struct pkt_desc** pkts = realloc(ptr, (*size + step) * sizeof(struct pkt_desc*));
	if (pkts) {
		memset(pkts + *size, 0, sizeof(struct pkt_desc*) * step);
		*size += step;
	}
	return pkts;
}

void
free_descs(struct pkt_desc** pkts) {
	if (!pkts)
		return;

	size_t i = 0;
	while (pkts[i] != NULL) {
		free(pkts[i]);
	}
	free(pkts);
}

struct pkt_desc**
pkts_read(pcap_t *handle) {
	size_t size = 0;
	struct pkt_desc** pkts = NULL;

	size_t i = 0;
	struct pcap_pkthdr hdr;
	const u_char* pkt = NULL;
	while ((pkt = pcap_next(handle, &hdr)) != NULL) {
		if (i == size) {
			struct pkt_desc** tmp = pkts;
			pkts = realloc_descs(pkts, &size);
			if (!pkts) {
				free_descs(tmp);
				return NULL;
			}
			printf("Readed %llu packets\n", i);
		}

		pkts[i] = malloc(sizeof(struct pkt_desc) + hdr.caplen);
		if (!pkts[i]) {
			free_descs(pkts);
			return NULL;
		}

		pkts[i]->hdr = hdr;
		memcpy(pkts[i]->data, pkt, hdr.caplen);
		++i;
	}
	return pkts;
}

uint64_t
GetUSec() {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((uint64_t)ts.tv_sec) * 1000000LL + ts.tv_nsec / 1000;
}

void
filter_pcap_file(const char *filename, const char *bpf_filter_str) {
	pcap_t *handle;
	char errbuf[PCAP_ERRBUF_SIZE];
	struct bpf_program bpf;
	struct filter_stats stats = {0, 0};
	int net = 0;
	struct pkt_desc** pkts = NULL;
	uint64_t start, end;
	size_t i = 0;

	if (bpf_filter_str == NULL || strlen(bpf_filter_str) == 0) {
		fprintf(stderr, "Unsupported pcap\n");
		return;
	}

	handle = pcap_open_offline(filename, errbuf);
	if (handle == NULL) {
		fprintf(stderr, "Open error %s: %s\n", filename, errbuf);
		return;
	}

	net = pcap_datalink(handle);
	if (pcap_compile(handle, &bpf, bpf_filter_str, OPTIMIZE_BPF, net) == -1) {
		fprintf(stderr, "Unsupported filter: %s\n", pcap_geterr(handle));
		pcap_close(handle);
		return;
	}

	pkts = pkts_read(handle);
	if (!pkts) {
		fprintf(stderr, "Error of read pkts\n");
		pcap_close(handle);
		return;
	}

	pcap_close(handle);

	printf("Start filtering...\n");

	start = GetUSec();
	while (pkts[i] != NULL) {
		stats.filtered_packets++;
		stats.bytes_total += pkts[i]->hdr.len;

		if (check_filter(pkts[i]->data, pkts[i]->hdr.caplen))
			stats.true_filtered++;

		// if (pcap_offline_filter(&bpf, &pkts[i]->hdr, pkts[i]->data))
		// 	stats.true_filtered++;

		++i;
	}
	end = GetUSec();
	pcap_freecode(&bpf);

	printf("Time (usecs): %llu\n", end - start);
	printf("Filtered (all): %llu\n", stats.filtered_packets);
	printf("Filtered (true): %llu\n", stats.true_filtered);
	printf("Speed in usec: %lf\n", ((double)(stats.filtered_packets)) / (end - start));
}

int
main(int argc, char *argv[]) {
	const char *filename;
	const char *bpf_filter = NULL;

	if (argc < 2)
		return 1;

	filename = argv[1];
	bpf_filter = FILTER;

	filter_pcap_file(filename, bpf_filter);

	return 0;
}