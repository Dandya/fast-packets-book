
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>

#include "settings.h"
#include "tests.h"

extern int read_ebpf_elf(const char *path, struct bpf_insn** insns, uint32_t* count);

enum bpftest_bpf_type {
	BPFTEST_FILTER = 0,
	BPFTEST_PROGRAM = 1
};

struct bpftest_ioctl_bpf_info {
	uint16_t insns_len;
	uint64_t insns;
	uint32_t pkts_descs_count;
	uint32_t pkts_descs_len;
	uint64_t pkts_descs;
	uint8_t type;
	uint8_t jit_enable;
	uint32_t tests_count;
	uint64_t stats;
};

#define IOCTL_DEVICE_NAME "bpftest"
#define IOCTL_CLASS_NAME "control"
#define BPFTEST_IOC_MAGIC 'b'

// Определение команды ioctl для модуля bpftest.
#define BPFTEST_IOCTL_RUN_BPF _IOWR(BPFTEST_IOC_MAGIC, 0, struct bpftest_ioctl_bpf_info)
#define BPFTEST_IOC_MAXNR 1

// Вспомогательная функция для включения/выключения JIT-компиляции BPF в ядре.
int
set_bpf_jit_enable(int value) {
	char buffer[2];
	int len, ret, fd;

	if (value < 0 && value > 2) {
		fprintf(stderr, "Invalide value for bpf_jit_enable: %d\n", value);
		return -1;
	}

	fd = open("/proc/sys/net/core/bpf_jit_enable", O_WRONLY);
	if (fd < 0) {
		fprintf(stderr, "Error open bpf_jit_enable: %s\n", strerror(errno));
		return -1;
	}

	len = snprintf(buffer, sizeof(buffer), "%d", value);	
	ret = write(fd, buffer, len);
	close(fd);
	
	if (ret != len) {
		fprintf(stderr, "Error write to bpf_jit_enable: %s\n", strerror(errno));
		return -1;
	}
	
	return 0;
}

// Функция запуска теста в модуле bpftest.
int
bpftest_run(struct bpftest_ioctl_bpf_info* info) {
	int fd, ret;
	char path[256];

	ret = set_bpf_jit_enable(info->jit_enable);
	if (ret < 0)
		return ret;
	
	snprintf(path, sizeof(path), "/dev/%s", IOCTL_DEVICE_NAME);

	fd = open(path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "Failed to open device %s: %s\n", path, strerror(errno));
		return -1;
	}

	ret = ioctl(fd, BPFTEST_IOCTL_RUN_BPF, info);
	if (ret < 0)
		fprintf(stderr, "ioctl failed: %s\n", strerror(errno));
	close(fd);
	return ret;
}

// Функция тестирования cBPF в ядре.
void
test_cbpf_kernel(struct pkt_descs_list* list, uint8_t is_jit) {
	pcap_t *handle;
	char errbuf[PCAP_ERRBUF_SIZE];
	struct bpf_program bpf;
	struct pcap_pkthdr hdr;
	struct filter_stats stats[TESTS_COUNT];
	struct bpftest_ioctl_bpf_info info;
	const char* name;
	int ret = 0;

	if (is_jit)
		name = "cBPF-jit-kernel";
	else
		name = "cBPF-virt-kernel";

	handle = pcap_open_dead(DLT_EN10MB, 1500);
	if (handle == NULL) {
		fprintf(stderr, "Open error pcap: %s\n", errbuf);
		return;
	}

	if (pcap_compile(handle, &bpf, FILTER, OPTIMIZE_BPF, DLT_EN10MB) == -1) {
		fprintf(stderr, "Unsupported filter: %s\n", pcap_geterr(handle));
		pcap_close(handle);
		return;
	}
	assert((bpf.bf_insns));

	pcap_close(handle);

	info.insns_len = (uint16_t)bpf.bf_len;
	info.insns = (uintptr_t)bpf.bf_insns;
	info.pkts_descs_count = list->count;
	info.pkts_descs_len = list->len;
	info.pkts_descs = (uintptr_t)list->descs;
	info.type = BPFTEST_FILTER;
	info.jit_enable = is_jit;
	info.tests_count = TESTS_COUNT;
	info.stats = (uintptr_t)stats;

	ret = bpftest_run(&info);
	pcap_freecode(&bpf);
	if (ret < 0) {
		fprintf(stderr, "Error of run test: %s\n", name);
		return;
	}

	print_stat(stats, name);
}

// Функция тестирования eBPF в ядре.
void
test_ebpf_kernel(struct pkt_descs_list* list, uint8_t is_jit) {
	struct filter_stats stats[TESTS_COUNT];
	struct bpftest_ioctl_bpf_info info;
	int ret = 0;
	struct bpf_insn* ebpf_insns;
	uint32_t count;
	const char* elf_file = "filter_ebpf.elf";
	const char* name;

	if (is_jit)
		name = "eBPF-jit-kernel";
	else
		name = "eBPF-virt-kernel";

	ret = read_ebpf_elf(elf_file, &ebpf_insns, &count);
	if (ret < 0) {
		fprintf(stderr, "Failed to read elf file: %s\n", elf_file);
		return;
	}

	if (count > UINT16_MAX) {
		free(ebpf_insns);
		fprintf(stderr, "Very big program: %s\n", elf_file);
		return;
	}

	info.insns_len = (uint16_t)count;
	info.insns = (uintptr_t)ebpf_insns;
	info.pkts_descs_count = list->count;
	info.pkts_descs_len = list->len;
	info.pkts_descs = (uintptr_t)list->descs;
	info.type = BPFTEST_PROGRAM;
	info.jit_enable = is_jit;
	info.tests_count = TESTS_COUNT;
	info.stats = (uintptr_t)stats;

	ret = bpftest_run(&info);
	free(ebpf_insns);
	if (ret < 0) {
		fprintf(stderr, "Error of run test: %s\n", name);
		return;
	}

	

	print_stat(stats, name);
}