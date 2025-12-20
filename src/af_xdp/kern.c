
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

int num_socks = 0;          // Количество доступных сокетов.
static unsigned int num_pkts = 0; // Количество захваченных пакетов.

SEC("xdp") // Расположение функции в секции "xdp_sock" ELF файла.
int xdp_sock_prog(struct xdp_md *ctx) { // Структура xdp_md хранит данные пакета.
	const char fmt[] = "NUM %d for %d\n";
  // Отправка каждого 10-го пакета вверх по сетевому стеку
  // и вывод информации о количестве захваченных пакетов.
  if ((++num_pkts % 10) == 0) {
    bpf_trace_printk(fmt, sizeof(fmt), num_pkts, num_socks);
    return XDP_PASS;
  }
  // Функция bpf_redirect_map заполняет ряд структур в ядре,
  // что в случае успеха возвращает значение XDP_REDIRECT.
  // Вызывающая сторона в случае значения XDP_REDIRECT вызывает
  // функцию передачи пакета в нужный сокет на основе заполненных структур.
  // Распределение происходит на основе номер логического процессора.
	return bpf_redirect_map(&xsks_map, bpf_get_smp_processor_id(), XDP_DROP);
}

char _license[] SEC("license") = "GPL";