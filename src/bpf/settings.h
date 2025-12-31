
#define TESTS_COUNT 1000

// Включение тестирование фильтра cBPF в интерпритаторе libpcap,
#define TEST_CBPF_LIBPCAP 0
// Включение тестирования фильтра выполненного в виде отдельной функции.
#define TEST_FUNCTION 0
// Включение тестирования фильтра cBPF преобразованного в отдельную функцию.
#define TEST_CBPF_FCC 1

// Включение тестирования фильтра cBPF в интерпритаторе ядра.
#define TEST_CBPF_VIRT_KERNEL 1
// Включение тестирования фильтра eBPF в интерпритаторе ядра.
#define TEST_EBPF_VIRT_KERNEL 1
// Включение тестирования фильтра cBPF после JIT-компиляции в ядре.
#define TEST_CBPF_JIT_KERNEL 1
// Включение тестирования фильтра eBPF после JIT-компиляции в ядре.
#define TEST_EBPF_JIT_KERNEL 1

// Включение тестирования фильтра cBPF в интерпритаторе DPDK.
#define TEST_CBPF_VIRT_DPDK 1
// Включение тестирования фильтра eBPF в интерпритаторе DPDK.
#define TEST_EBPF_VIRT_DPDK 1
// Включение тестирования фильтра cBPF после JIT-компиляции в DPDK.
#define TEST_CBPF_JIT_DPDK 1
// Включение тестирования фильтра eBPF после JIT-компиляции в DPDK
#define TEST_EBPF_JIT_DPDK 1

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
			(dst port 1900 and ip[9] == 0x11 and ip[8] == 0x40) \
		)) \
)"

