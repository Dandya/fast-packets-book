
#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>

// Основан на примере из:
//    https://github.com/xdp-project/bpf-examples/blob/main/AF_XDP-example/xdpsock_kern.c

// Максимальное количество сокетов для захвата пакетов.
#define MAX_SOCKS 32

struct {
	__uint(type, BPF_MAP_TYPE_XSKMAP); // Тип общей памяти, определяющий её реализацию.
	__uint(max_entries, MAX_SOCKS);    // Максимальное количество элементов.
	__uint(key_size, sizeof(int));     // Размер ключа для "словаря".
	__uint(value_size, sizeof(int));   // Размер значения, соответствующий некоторому ключу.
} xsks_map SEC(".maps");             // Расположение структуры в секции ".maps" ELF файла.

int num_socks = 0;      // Количество доступных сокетов.
static unsigned int rr; // Round-robin балансировка трафика.
static unsigned int num_pkts = 0;

SEC("xdp") // Расположение функции в секции "xdp_sock" ELF файла.
int xdp_sock_prog(struct xdp_md *ctx) { // Структура xdp_md хранит данные пакета.
	const char fmt[] = "NUM %d\n";
  // Вычисление следующего индекса сокета.
  num_pkts++;
  if (num_pkts % 10 == 0)
    bpf_trace_printk(fmt, sizeof(fmt), num_pkts);
  rr = (rr + 1) & (num_socks - 1);
  // Функция bpf_redirect_map заполняет ряд структур в ядре,
  // что в случае успеха возвращает значение XDP_REDIRECT.
  // Вызывающая сторона в случае значения XDP_REDIRECT вызывает
  // функцию передачи пакета в нужный сокет на основе заполненных структур.
	return bpf_redirect_map(&xsks_map, rr, XDP_DROP);
}

char _license[] SEC("license") = "GPL";