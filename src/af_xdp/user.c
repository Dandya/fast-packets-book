
#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <libgen.h>
#include <linux/bpf.h>
#include <linux/err.h>
#include <linux/if_link.h>
#include <linux/if_xdp.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/limits.h>
#include <linux/udp.h>
#include <arpa/inet.h>
#include <locale.h>
#include <net/ethernet.h>
#include <netinet/ether.h>
#include <net/if.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>

#include <xdp/xsk.h>
#include <xdp/libxdp.h>

#include <bpf/libbpf.h>
#include <bpf/bpf.h>

// Основан на примере:
//   https://github.com/xdp-project/bpf-examples/blob/main/AF_XDP-example/xdpsock.c

// Определение флага продолжения фрагмента, если его нет в заголовках.
#ifndef XDP_PKT_CONTD
#define XDP_PKT_CONTD (1 << 0)
#endif
// Определение флага последнего фрагмента пакета.
#define IS_EOP_DESC(options) (!((options) & XDP_PKT_CONTD))

// Максимальное количество сокетов для захвата пакетов.
// Взято на основе максимального количества логических процессоров.
#define MAX_SOCKS 32
// Количество повторов для отправки пакетов.
#define TX_RETRIES_COUNT 3


// Настройки очереди.
#define NUM_FRAMES (4 * 1024) // Количество ячеек для записи пакетов/фрагментов.
#define MIN_PKT_SIZE 64       // Минимальная длина пакета.
#define MAX_PKT_SIZE 9728     // Максимальная длина пакета на основе возможностей сетевой карты.

// Флаг вывода данных о полученных пакетах.
#define DEBUG_HEXDUMP 1

// Режимы работы программы.
enum mode_type {
	MODE_RXONLY = 0,
	MODE_TXONLY = 1,
};

// Структура с данными о кольце UMEM.
struct umem_info {
	struct xsk_ring_prod pr;
	struct xsk_ring_cons cr;
	struct xsk_umem* umem;
	void* buffer;
};

// Структура c данными о сокете.
struct socket_info {
	struct xsk_ring_cons rx;
	struct xsk_ring_prod tx;
	struct xsk_socket* xsk;
	struct umem_info* umem;
	uint64_t tx_count;
	uint64_t rx_count;
};

// Режим работы XDP.
static enum xdp_attach_mode opt_attach_mode = XDP_MODE_NATIVE;
// Режим работы программы.
static enum mode_type opt_mode = MODE_RXONLY;
// Название интерфейса.
static const char* opt_if = "";
// Индекс интерфейса.
static int opt_ifindex;
// Флаг окончания работы.
static bool work_done;
// Размер блока пакетов для принятия/отправки.
static uint32_t opt_batch_size = 64;
// Количество пакетов для отправки.
static int opt_pkt_count;
// Размер пакета/фрагмента в очереди.
static uint16_t opt_pkt_size = MIN_PKT_SIZE;
// Флаг ожидания пакетов с помощью системного вызова poll.
static int opt_poll;
// Флаги настройки работы xdp.
static uint32_t opt_xdp_bind_flags = XDP_USE_NEED_WAKEUP;
// Флаги настройки работы кольца umem.
static uint32_t opt_umem_flags = 0;
// Флаг использования невыровненных пакетов.
static int opt_unaligned_chunks = 0;
// Флаги настройки системного вызова mmap.
static int opt_mmap_flags = 0;
// Длина пакета/фрагмента в кольце UMEM.
static int opt_xsk_frame_size = XSK_UMEM__DEFAULT_FRAME_SIZE;
// Необходимое количество ячеек фрагментов для отправки пакета.
static int frames_per_pkt;
// Длина таймаута для ожидания пакетов в миллисекундах.
static int opt_timeout = 1000;
// Флаг ручного уведомления ядра о новых пакетах при отправке.
static bool opt_need_wakeup = true;
// Количество очередей приёма пакетов.
static uint32_t opt_num_xsks = 1;
// Флаг режима "busy_poll".
static bool opt_busy_poll = false;
// Флаг необходимости установки XDP в ядро.
static bool opt_load_xdp = false;
// Путь до XDP программы.
static const char* opt_xdp_path = "";
// Указатель на загруженную в ядро XDP.
static struct xdp_program* xdp_prog = NULL;

// Количество попыток отправки пакетов перед ошибкой.
static int retries_count = TX_RETRIES_COUNT;

// Данные для отправки пакета.
char syn_pkt[] = {
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
}; // 192.168.1.2	192.168.0.10	TCP	74	35980 → 80 [SYN] Seq=0 Win=64240

// Количество открытых сокетов.
static int num_socks = 0;
// Массив с данными о сокетах.
static struct socket_info* xsks[MAX_SOCKS];

// Функция обработки сигналов.
static void int_exit(int sig) {
	work_done = true;
}

// Функция вывода пакета в hex виде.
static void
hex_dump(void* pkt, size_t length, uint64_t addr) {
	const unsigned char* address = (unsigned char*)pkt;
	const unsigned char* line = address;
	size_t line_size = 32;
	unsigned char c;
	char buf[32];
	int i = 0;

	if (!DEBUG_HEXDUMP)
		return;

	sprintf(buf, "addr=%llu", addr);
	printf("length = %zu\n", length);
	printf("%s | ", buf);
	while (length-- > 0) {
		printf("%02X ", *address++);
		if (!(++i % line_size) || (length == 0 && i % line_size)) {
			if (length == 0) {
				while (i++ % line_size)
					printf("__ ");
			}
			printf(" | ");
			while (line < address) {
				c = *line++;
				printf("%c", (c < 33 || c == 255) ? 0x2E : c);
			}
			printf("\n");
			if (length > 0)
				printf("%s | ", buf);
		}
	}
	printf("\n");
}

// Функция получения времени в наносекундах.
// Подробнее: https://man7.org/linux/man-pages/man3/clock_gettime.3.html
static unsigned long
get_nsecs(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000UL + ts.tv_nsec;
}

// Функция удаления XDP из ядра.
static void
remove_xdp_program(void) {
	// Удаление XDP программы из сетевого интерфейса.
	// Подробнее: https://docs.ebpf.io/ebpf-library/libbpf/userspace/bpf_xdp_detach
	int err = xdp_program__detach(xdp_prog, opt_ifindex, opt_attach_mode, 0);
	if (err)
		fprintf(stderr, "Could not detach XDP program. Error: %s\n", strerror(-err));
}

// Функция завершения работы программы с выводом отладочной информации.
static void
__exit_with_error(int error, const char* file, const char* func,
		int line) {
	fprintf(stderr, "%s:%s:%i: errno: %d/\"%s\"\n", file, func,
		line, error, strerror(error));

	if (opt_load_xdp)
		remove_xdp_program();
	exit(EXIT_FAILURE);
}
#define exit_with_error(error) __exit_with_error(error, __FILE__, __func__, __LINE__)

// Функция загрузки XDP в ядро.
static void
load_xdp_program(void) {
	char errmsg[1024];
	int err;

	if (!opt_load_xdp)
		return;

	// Чтение объектного файла с байткодом eBPT.
	// Подробнее: https://docs.ebpf.io/ebpf-library/libxdp/functions/xdp_program__open_file
	xdp_prog = xdp_program__open_file(opt_xdp_path, NULL, NULL);
	err = libxdp_get_error(xdp_prog);
	if (err) {
		libxdp_strerror(err, errmsg, sizeof(errmsg));
		fprintf(stderr, "ERROR: program loading failed: %s\n", errmsg);
		exit(EXIT_FAILURE);
	}

	// Установка XDP программы в ядро и закрепление её за очередью сетевого интерфейса.
	err = xdp_program__attach(xdp_prog, opt_ifindex, opt_attach_mode, 0);
	if (err) {
		libxdp_strerror(err, errmsg, sizeof(errmsg));
		fprintf(stderr, "ERROR: attaching program failed: %s\n", errmsg);
		exit(EXIT_FAILURE);
	}
}

// Поиск "map" с названием "xsks_map".
static int
lookup_bpf_map(int prog_fd) {
	__uint32_t i, *map_ids, num_maps, prog_len = sizeof(struct bpf_prog_info);
	__uint32_t map_len = sizeof(struct bpf_map_info);
	struct bpf_prog_info prog_info = {};
	int fd, err, xsks_map_fd = -ENOENT;
	struct bpf_map_info map_info;

	// Получение количества "maps" в XDP программе.
	// Подробнее: https://docs.ebpf.io/ebpf-library/libbpf/userspace/bpf_obj_get_info_by_fd
	err = bpf_obj_get_info_by_fd(prog_fd, &prog_info, &prog_len);
	if (err)
		return err;

	num_maps = prog_info.nr_map_ids;

	map_ids = calloc(prog_info.nr_map_ids, sizeof(*map_ids));
	if (!map_ids)
		return -ENOMEM;

	memset(&prog_info, 0, prog_len);
	prog_info.nr_map_ids = num_maps;
	prog_info.map_ids = (__uint64_t)(unsigned long)map_ids;

	// Получение информации о "maps" XDP программе.
	// Подробнее: https://docs.ebpf.io/ebpf-library/libbpf/userspace/bpf_obj_get_info_by_fd
	err = bpf_obj_get_info_by_fd(prog_fd, &prog_info, &prog_len);
	if (err) {
		free(map_ids);
		return err;
	}

	for (i = 0; i < prog_info.nr_map_ids; i++) {
		// Получение файлового дескриптора "map".
		// Подробнее: https://docs.ebpf.io/ebpf-library/libbpf/userspace/bpf_map_get_fd_by_id/
		fd = bpf_map_get_fd_by_id(map_ids[i]);
		if (fd < 0)
			continue;

		memset(&map_info, 0, map_len);
		// Получение информации об отдельной "map".
		// Подробнее: https://docs.ebpf.io/ebpf-library/libbpf/userspace/bpf_obj_get_info_by_fd/
		err = bpf_obj_get_info_by_fd(fd, &map_info, &map_len);
		if (err) {
			close(fd);
			continue;
		}

		if (!strncmp(map_info.name, "xsks_map", sizeof(map_info.name)) &&
				map_info.key_size == 4 && map_info.value_size == 4) {
			xsks_map_fd = fd;
			break;
		}

		close(fd);
	}

	free(map_ids);
	return xsks_map_fd;
}

// Функция добавления в "map" с названием "xsks_map" файловых дескрипторов сокетов.
static void
enter_xsks_into_map(void) {
	struct bpf_map* data_map;
	int xsks_map;
	int key = 0;

	// Получение "map", представляющую .bss раздел объектного файла.
	// Подробнее: https://docs.ebpf.io/ebpf-library/libbpf/userspace/bpf_object__find_map_by_name/
	data_map = bpf_object__find_map_by_name(xdp_program__bpf_obj(xdp_prog), ".bss");
	if (!data_map || !bpf_map__is_internal(data_map)) {
		fprintf(stderr, "ERROR: bss map found!\n");
		exit(EXIT_FAILURE);
	}
	// Обновление количества сокетов (переменной `num_socks`) в XDP программе.
	// Подробнее: https://docs.ebpf.io/ebpf-library/libbpf/userspace/bpf_map_update_elem/
	if (bpf_map_update_elem(bpf_map__fd(data_map), &key, &num_socks, BPF_ANY)) {
		fprintf(stderr, "ERROR: bpf_map_update_elem num_socks %d!\n", num_socks);
		exit(EXIT_FAILURE);
	}

	// Получение файлового дескриптора "map" с названием "xsks_map".
	xsks_map = lookup_bpf_map(xdp_program__fd(xdp_prog));
	if (xsks_map < 0) {
		fprintf(stderr, "ERROR: no xsks map found: %s\n",
			strerror(xsks_map));
			exit(EXIT_FAILURE);
	}

	// Обновление значений файловых дескрипторов сокетов в "xsks_map".
	for (int i = 0; i < num_socks; i++) {
		int fd = xsk_socket__fd(xsks[i]->xsk);
		int ret;

		key = i;
		ret = bpf_map_update_elem(xsks_map, &key, &fd, 0);
		if (ret) {
			fprintf(stderr, "ERROR: bpf_map_update_elem %d\n", i);
			exit(EXIT_FAILURE);
		}
	}
}

// Функция настройки сокетов.
static void
apply_setsockopt(struct socket_info* xsk) {
	int sock_opt;

	if (!opt_busy_poll)
		return;

	// Установка 'busy-poll' режима для сокета.
	sock_opt = 1;
	if (setsockopt(xsk_socket__fd(xsk->xsk), SOL_SOCKET, SO_PREFER_BUSY_POLL,
			(void*)&sock_opt, sizeof(sock_opt)) < 0)
		exit_with_error(errno);

	sock_opt = 20;
	if (setsockopt(xsk_socket__fd(xsk->xsk), SOL_SOCKET, SO_BUSY_POLL,
			(void*)&sock_opt, sizeof(sock_opt)) < 0)
		exit_with_error(errno);

	sock_opt = opt_batch_size;
	if (setsockopt(xsk_socket__fd(xsk->xsk), SOL_SOCKET, SO_BUSY_POLL_BUDGET,
			(void*)&sock_opt, sizeof(sock_opt)) < 0)
		exit_with_error(errno);
}


// Функция завершения работы сокетов.
static void
socks_cleanup(void) {
	int ret = 0;
	printf("\n");
	for (int i = 0; i < num_socks; i++) {
		printf("Socket %d:\t %llu Rx,\t %llu Tx\n", i, xsks[i]->rx_count, xsks[i]->tx_count);
		// Удаление сокета.
		// Подробнее: https://docs.ebpf.io/ebpf-library/libxdp/functions/xsk_socket__delete/
		xsk_socket__delete(xsks[i]->xsk);
		// Очистка UMEM кольца.
		// Подробнее: https://docs.ebpf.io/ebpf-library/libxdp/functions/xsk_umem__delete/
		ret = xsk_umem__delete(xsks[i]->umem->umem);
		if (ret < 0)
			exit_with_error(ret);
		munmap(xsks[i]->umem->buffer, NUM_FRAMES * opt_xsk_frame_size);
	}

	if (opt_load_xdp)
		remove_xdp_program();
}

// Функция создания кольца UMEM.
// Подробнее: https://docs.ebpf.io/ebpf-library/libxdp/functions/xsk_umem__create/
static struct umem_info*
create_umem(void* buffer, uint64_t size) {
	struct umem_info* umem;
	struct xsk_umem_config cfg = {
		.fill_size = XSK_RING_PROD__DEFAULT_NUM_DESCS * 2,
		.comp_size = XSK_RING_CONS__DEFAULT_NUM_DESCS,
		.frame_size = opt_xsk_frame_size,
		.frame_headroom = XSK_UMEM__DEFAULT_FRAME_HEADROOM,
		.flags = opt_umem_flags
	};
	int ret;

	umem = calloc(1, sizeof(*umem));
	if (!umem)
		exit_with_error(errno);

	ret = xsk_umem__create(&umem->umem, buffer, size, &umem->pr, &umem->cr, &cfg);
	if (ret)
		exit_with_error(-ret);

	umem->buffer = buffer;
	return umem;
}

// Функция заполнения очереди fill (производитель для пользователя) свободными дескрипторами,
// для последующего заполнения их ядром.
static void
configure_fill_ring(struct umem_info* umem) {
	int ret = 0;
	uint32_t idx = 0;

	// Резервирование слотов в очереди fill.
	// Подробнее: https://docs.ebpf.io/ebpf-library/libxdp/functions/xsk_ring_prod__reserve
	ret = xsk_ring_prod__reserve(&umem->pr, XSK_RING_PROD__DEFAULT_NUM_DESCS * 2, &idx);
	if (ret != XSK_RING_PROD__DEFAULT_NUM_DESCS * 2)
		exit_with_error(-ret);
	// Запись в очередь смещенией в UMEM для записи по ним пакетов.
	// Подробнее: https://docs.ebpf.io/ebpf-library/libxdp/functions/xsk_ring_prod__fill_addr
	for (int i = 0; i < XSK_RING_PROD__DEFAULT_NUM_DESCS * 2; i++)
		*xsk_ring_prod__fill_addr(&umem->pr, idx++) = i * opt_xsk_frame_size;
	// Указание ядру о готовности очереди.
	// Подробнее: https://docs.ebpf.io/ebpf-library/libxdp/functions/xsk_ring_prod__submit
	xsk_ring_prod__submit(&umem->pr, XSK_RING_PROD__DEFAULT_NUM_DESCS * 2);
}

// Функция создания сокета.
static struct socket_info*
create_socket(struct umem_info* umem,
		bool rx, bool tx, int queue_id) {
	struct xsk_socket_config cfg;
	struct socket_info* xsk;
	struct xsk_ring_cons* rxr;
	struct xsk_ring_prod* txr;
	int ret;

	xsk = calloc(1, sizeof(*xsk));
	if (!xsk)
		exit_with_error(errno);

	xsk->umem = umem;
	cfg.rx_size = XSK_RING_CONS__DEFAULT_NUM_DESCS;
	cfg.tx_size = XSK_RING_PROD__DEFAULT_NUM_DESCS;
	if (opt_load_xdp) {
		// Отключение загрузки xdp по умолчанию.
		cfg.libxdp_flags = XSK_LIBXDP_FLAGS__INHIBIT_PROG_LOAD;
	} else {
		cfg.libxdp_flags = 0;
	}
	if (opt_attach_mode == XDP_MODE_SKB) {
		cfg.xdp_flags = XDP_FLAGS_SKB_MODE;
	} else {
		cfg.xdp_flags = XDP_FLAGS_DRV_MODE;
	}
	cfg.bind_flags = opt_xdp_bind_flags;

	rxr = rx ? &xsk->rx : NULL;
	txr = tx ? &xsk->tx : NULL;
	// Создание сокета на очереди `queue_id` сетевого интерфейса c индексом `opt_if`.
	// Подробнее: https://docs.ebpf.io/ebpf-library/libxdp/functions/xsk_socket__create
	ret = xsk_socket__create(&xsk->xsk, opt_if, queue_id, umem->umem, rxr, txr, &cfg);
	if (ret)
		exit_with_error(-ret);

	return xsk;
}

// Функция записи пакета в очередь UMEM.
static void
gen_eth_frame(struct umem_info* umem, uint64_t addr, uint32_t* len) {
	uint32_t copy_len = opt_xsk_frame_size;
	if (!*len)
		*len = sizeof(syn_pkt);
	if (*len < opt_xsk_frame_size)
		copy_len = *len;

	memcpy(xsk_umem__get_data(umem->buffer, addr), syn_pkt + sizeof(syn_pkt) - *len, copy_len);
	*len -= copy_len;
}

// Функция получения пакетов.
static void
rx_only(struct socket_info* xsk) {
	unsigned int rcvd, i, eop_cnt = 0;
	uint32_t idx_rx = 0, idx_fq = 0;
	int ret;

	// Просмотр количества доступных пакетов/фрагментов для чтения.
	// Подробнее: https://docs.ebpf.io/ebpf-library/libxdp/functions/xsk_ring_cons__peek/
	rcvd = xsk_ring_cons__peek(&xsk->rx, opt_batch_size, &idx_rx);
	if (!rcvd) {
		// if (opt_busy_poll || xsk_ring_prod__needs_wakeup(&xsk->umem->pr)) {
		// 	// Уведомление ядра об ожидании пакетов.
		// 	// Подробнее: https://man7.org/linux/man-pages/man3/recvfrom.3p.html
		// 	recvfrom(xsk_socket__fd(xsk->xsk), NULL, 0, MSG_DONTWAIT, NULL, NULL);
		// }
		return;
	}

	// Резервирование дескрипторов для записи прочитанных пакетов/фрагментов.
	// Подробнее: https://docs.ebpf.io/ebpf-library/libxdp/functions/xsk_ring_prod__reserve/
	ret = xsk_ring_prod__reserve(&xsk->umem->pr, rcvd, &idx_fq);
	while (ret != rcvd) {
		if (ret < 0)
			exit_with_error(-ret);
		if (opt_busy_poll || xsk_ring_prod__needs_wakeup(&xsk->umem->pr)) {
			// Уведомление ядра об ожидании пакетов.
			// Подробнее: https://man7.org/linux/man-pages/man3/recvfrom.3p.html
			recvfrom(xsk_socket__fd(xsk->xsk), NULL, 0, MSG_DONTWAIT, NULL, NULL);
		}
		ret = xsk_ring_prod__reserve(&xsk->umem->pr, rcvd, &idx_fq);
	}

	// Чтение пакетов.
	for (i = 0; i < rcvd; i++) {
		// Получение дескриптора с данными.
		const struct xdp_desc* desc = xsk_ring_cons__rx_desc(&xsk->rx, idx_rx++);
		uint64_t addr = desc->addr;
		uint32_t len = desc->len;
		// Получение адреса начала доступных данных в UMEM.
		// Подробнее: https://docs.ebpf.io/ebpf-library/libxdp/functions/xsk_umem__extract_addr/
		uint64_t orig = xsk_umem__extract_addr(addr);
		eop_cnt += IS_EOP_DESC(desc->options);

		// Получение адреса начала данных.
		// Подробнее: https://docs.ebpf.io/ebpf-library/libxdp/functions/xsk_umem__add_offset_to_addr/
		addr = xsk_umem__add_offset_to_addr(addr);

		// Получение данных пакета.
		// Подробнее: https://docs.ebpf.io/ebpf-library/libxdp/functions/xsk_umem__get_data/
		char* pkt = xsk_umem__get_data(xsk->umem->buffer, addr);

		hex_dump(pkt, len, addr);
		// Установка адреса данных для последующей записи по нему данных.
		*xsk_ring_prod__fill_addr(&xsk->umem->pr, idx_fq++) = orig;
	}

	xsk->rx_count += rcvd;

	// Освобождение дескрипторов в очереди дескрипторов доступных для чтения.
	// Подробнее: https://docs.ebpf.io/ebpf-library/libxdp/functions/xsk_ring_prod__submit/
	xsk_ring_prod__submit(&xsk->umem->pr, rcvd);
	// Создание дескрипторов в очереди дескрипторов доступных для записи.
	// Подробнее: https://docs.ebpf.io/ebpf-library/libxdp/functions/xsk_ring_cons__release/
	xsk_ring_cons__release(&xsk->rx, rcvd);
}

// Функция чтения и ожидания пакетов.
static void
rx_only_all(struct socket_info* xsk) {
	struct pollfd poll_fd;
	int ret;

	poll_fd.fd = xsk_socket__fd(xsk->xsk);
	poll_fd.events = POLLIN;

	while (true) {
		if (work_done)
			break;

		if (opt_poll) {
			// Системный вызов ожидания событий получения пакетов.
			// Подробнее: https://man7.org/linux/man-pages/man2/poll.2.html
			ret = poll(&poll_fd, 1, opt_timeout);
			if (!(poll_fd.revents & POLLIN))
				continue;
		}

		rx_only(xsk);

		if (work_done)
			break;
	}
}

// Функция получение длины набора пакетов/фрагментов для отправки.
static inline int
get_batch_size(int pkt_cnt) {
	if (!opt_pkt_count || pkt_cnt + opt_batch_size <= opt_pkt_count)
		return opt_batch_size * frames_per_pkt;

	return (opt_pkt_count - pkt_cnt) * frames_per_pkt;
}

// Функция отправки пакетов в ожидающем режиме ядра.
static void
kick_tx(struct socket_info* xsk) {
	int ret;
	// Системный вызов отправки данных ядру.
	// Подробнее: https://man7.org/linux/man-pages/man3/sendto.3p.html
	ret = sendto(xsk_socket__fd(xsk->xsk), NULL, 0, MSG_DONTWAIT, NULL, 0);

	if (ret >= 0 || errno == ENOBUFS || errno == EAGAIN ||
	    errno == EBUSY || errno == ENETDOWN)
		return;
	exit_with_error(errno);
}

// Функция отправки записанных пакетов.
static inline unsigned int
complete_tx_only(struct socket_info* xsk, int batch_size) {
	unsigned int rcvd;
	uint32_t idx;

	if (!opt_need_wakeup || xsk_ring_prod__needs_wakeup(&xsk->tx))
		kick_tx(xsk);

	// Получение количества освободивщихся/отправленных пакетов.
	// Подробнее: https://docs.ebpf.io/ebpf-library/libxdp/functions/xsk_ring_cons__peek/
	rcvd = xsk_ring_cons__peek(&xsk->umem->cr, batch_size, &idx);
	if (rcvd > 0) {
		// Возвращаем ядру дескрипторы, которые он отправил, чтобы записать в них данные заного.
		// Подробнее: https://docs.ebpf.io/ebpf-library/libxdp/functions/xsk_ring_cons__peek/
		xsk_ring_cons__release(&xsk->umem->cr, rcvd);
	}
  return rcvd;
}

// Функция записи и отправки `batch_size` пакетов.
static int
tx_only(struct socket_info* xsk, uint32_t* frame_nb, int batch_size) {
	uint32_t idx, tv_sec, tv_usec;
	unsigned int i;

	// Резервирование `batch_size` пакетов/фрагментов в очереди записи.
	// Подробнее: https://docs.ebpf.io/ebpf-library/libxdp/functions/xsk_ring_prod__reserve/
	while (xsk_ring_prod__reserve(&xsk->tx, batch_size, &idx) < batch_size) {
		complete_tx_only(xsk, batch_size);
		if (work_done)
			return 0;
	}

	for (i = 0; i < batch_size;) {
		uint32_t len = sizeof(syn_pkt);

		do {
			// Получение дексриптора из очереди записи.
			struct xdp_desc* tx_desc = xsk_ring_prod__tx_desc(&xsk->tx,
									  idx + i);
			tx_desc->addr = *frame_nb * opt_xsk_frame_size;
			if (len > opt_xsk_frame_size) {
				tx_desc->len = opt_xsk_frame_size;
				tx_desc->options = XDP_PKT_CONTD;
			} else {
				tx_desc->len = len;
				tx_desc->options = 0;
			}
			len -= tx_desc->len;
			*frame_nb = (*frame_nb + 1) % NUM_FRAMES;
			i++;
		} while (len);
	}

	// Подтверждение записи пакетов/фрагментов.
	// Подтверждение: https://docs.ebpf.io/ebpf-library/libxdp/functions/xsk_ring_prod__submit/
	xsk_ring_prod__submit(&xsk->tx, batch_size);
	xsk->tx_count += batch_size;
	// Ожидание отправки пакетов/фрагментов ядром.
	complete_tx_only(xsk, batch_size);

	return batch_size / frames_per_pkt;
}

// Функция заверщения отправки пакетов.
static void
complete_tx_only_all(struct socket_info* xsk)
{
	bool pending;

	do {
		pending = false;
		if (xsk->tx_count) {
			complete_tx_only(xsk, opt_batch_size);
			pending = !!xsk->tx_count;
		}
		sleep(1);
	} while (pending && retries_count-- > 0);
}

// Функция отправки пакетов.
static void
tx_only_all(struct socket_info* xsk)
{
	struct pollfd poll_fd;
	uint32_t frame_nb = 0;
	int pkt_cnt = 0;
	int ret;

	poll_fd.fd = xsk_socket__fd(xsk->xsk);
	poll_fd.events = POLLOUT;

	while ((opt_pkt_count && pkt_cnt < opt_pkt_count) || !opt_pkt_count) {
		int batch_size = get_batch_size(pkt_cnt);
		int tx_cnt = 0;

		if (work_done)
			break;

		if (opt_poll) {
			// Системный вызов ожидания событий отправки пакетов.
			// Подробнее: https://man7.org/linux/man-pages/man2/poll.2.html
			ret = poll(&poll_fd, 1, opt_timeout);

			if (!(poll_fd.revents & POLLOUT))
				continue;
		}

		tx_cnt += tx_only(xsk, &frame_nb, batch_size);

		pkt_cnt += tx_cnt;
	}

	if (opt_pkt_count)
		complete_tx_only_all(xsk);
}

// Доступные аргументы программы.
static struct option long_options[] = {
	{"rxonly", no_argument, 0, 'r'},
	{"txonly", no_argument, 0, 't'},
	{"interface", required_argument, 0, 'i'},
	{"queue", required_argument, 0, 'q'},
	{"load-xdp", required_argument, 0, 'l'},
	{"poll", no_argument, 0, 'p'},
	{"busy-poll", no_argument, 0, 'b'},
	{"xdp-skb", no_argument, 0, 'S'},
	{"xdp-native", no_argument, 0, 'N'},
	{"frame-size", required_argument, 0, 'f'},
	{"no-need-wakeup", no_argument, 0, 'm'},
	{"unaligned", no_argument, 0, 'u'},
	{"frags", no_argument, 0, 'F'},
	{"batch-size", required_argument, 0, 's'},
	{"tx-pkt-count", required_argument, 0, 'C'},
	{0, 0, 0, 0}
};

// Функиция вывода информации о поддерживаемых аргументах.
static void usage(const char* prog) {
	const char* str =
		"  Usage: %s [OPTIONS]\n"
		"  Options:\n"
		"  -r, --rxonly		Print all incoming packets (default)\n"
		"  -t, --txonly		Only send packets\n"
		"  -i, --interface=<NAME>	Run on interface n\n"
		"  -q, --queues=n	Use n queue (default 1)\n"
		"  -l, --load-xdp	Load xdp programm\n"
		"  -p, --poll		Use poll syscall\n"
		"  -b, --busy-poll Use busy-poll mode\n"
		"  -S, --xdp-skb	Use XDP skb-mod\n"
		"  -N, --xdp-native	Enforce XDP native mode\n"
		"  -m, --no-need-wakeup Turn off use of driver need wakeup flag.\n"
		"  -f, --frame-size=n   Set the frame size (must be a power of two in aligned mode, default is %d).\n"
		"  -u, --unaligned	Enable unaligned chunk placement\n"
		// "  -F, --frags		Enable frags (multi-buffer) support\n"
		"  -s, --batch-size=n	Batch size for sending or receiving\n"
		"			packets. Default: %d\n"
		"  -C, --tx-pkt-count=n	Number of packets to send.\n"
		"			Default: Continuous packets.\n"
		"\n";
	fprintf(stderr, str, prog, XSK_UMEM__DEFAULT_FRAME_SIZE, opt_batch_size);

	exit(EXIT_FAILURE);
}

// Функция парсинга аргументов командной строки.
static void
parse_command_line(int argc, char** argv) {
	int option_index, c;

	opterr = 0;

	for (;;) {
		c = getopt_long(argc, argv,
				"rti:q:pSNf:muMb:C:Fl:",
				long_options, &option_index);
		if (c == -1)
			break;

		switch (c) {
		case 'r':
			opt_mode = MODE_RXONLY;
			break;
		case 't':
			opt_mode = MODE_TXONLY;
			break;
		case 'i':
			opt_if = optarg;
			break;
		case 'q':
			opt_num_xsks = atoi(optarg);
			break;
		case 'l':
			opt_load_xdp = true;
			opt_xdp_path = optarg;
			break;
		case 'p':
			opt_poll = 1;
			break;
		case 'b':
			opt_busy_poll = 1;
			break;
		case 'S':
			opt_attach_mode = XDP_MODE_SKB;
			opt_xdp_bind_flags |= XDP_COPY;
			break;
		case 'N':
			opt_xdp_bind_flags |= XDP_ZEROCOPY;
			break;
		case 'u':
			opt_umem_flags |= XDP_UMEM_UNALIGNED_CHUNK_FLAG;
			opt_unaligned_chunks = 1;
			opt_mmap_flags = MAP_HUGETLB;
			break;
		case 'f':
			opt_xsk_frame_size = atoi(optarg);
			break;
		case 'm':
			opt_need_wakeup = false;
			opt_xdp_bind_flags &= ~XDP_USE_NEED_WAKEUP;
			break;
		case 's':
			opt_batch_size = atoi(optarg);
			break;
		case 'C':
			opt_pkt_count = atoi(optarg);
			break;
		default:
			usage(basename(argv[0]));
		}
	}

	// Получение индекса сетевого интерфейса.
	// Подробнее: https://man7.org/linux/man-pages/man3/if_indextoname.3.html
	opt_ifindex = if_nametoindex(opt_if);
	if (!opt_ifindex) {
		fprintf(stderr, "ERROR: interface \"%s\" does not exist\n", opt_if);
		usage(basename(argv[0]));
	}

	// Проверка, что длина пакета/фрагмента является степенью двойки.
	if ((opt_xsk_frame_size & (opt_xsk_frame_size - 1)) && !opt_unaligned_chunks) {
		fprintf(stderr, "--frame-size=%d is not a power of two\n", opt_xsk_frame_size);
		usage(basename(argv[0]));
	}
}


void*
run_af_xdp(void* ptr) {
	int i = *(int*)ptr;
	if (opt_mode == MODE_RXONLY)
		rx_only_all(xsks[i]);
	else if (opt_mode == MODE_TXONLY)
		tx_only_all(xsks[i]);
}

int
set_affinity_attr(pthread_attr_t *attr, int cpu) {
	int ret = 0;
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(cpu, &cpuset);

	if ((ret	= pthread_attr_setaffinity_np(attr, sizeof(cpu_set_t), &cpuset)))
		printf("Set affinity attribute for core %d: %s\n", cpu, strerror(ret));

	return (ret > 0) ? -ret : ret;
}

int main(int argc, char** argv) {
	struct umem_info* umem;
	int ret;
	void*bufs = NULL;

	pthread_t* threads = NULL;
	pthread_attr_t* attrs = NULL;
	int* args = NULL;

	parse_command_line(argc, argv);

	if (opt_num_xsks > MAX_SOCKS)
		exit_with_error(EINVAL);

	signal(SIGINT, int_exit);
	signal(SIGTERM, int_exit);
	signal(SIGABRT, int_exit);

  if (opt_load_xdp)
    load_xdp_program();

	threads = calloc(opt_num_xsks, sizeof(pthread_t));
	attrs = calloc(opt_num_xsks, sizeof(pthread_attr_t));
	args = calloc(opt_num_xsks, sizeof(int));
	if (!threads || !attrs || !args)
		exit_with_error(ENOMEM);

	frames_per_pkt = (sizeof(syn_pkt) - 1) / XSK_UMEM__DEFAULT_FRAME_SIZE + 1;

	for (int i = 0; i < opt_num_xsks; i++) {
		bufs = mmap(NULL, NUM_FRAMES * opt_xsk_frame_size, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS | opt_mmap_flags, -1, 0);
		if (bufs == MAP_FAILED) {
			printf("ERROR: mmap failed\n");
			exit(EXIT_FAILURE);
		}

		umem = create_umem(bufs, NUM_FRAMES * opt_xsk_frame_size);
		xsks[i] = create_socket(umem, opt_mode == MODE_RXONLY, opt_mode == MODE_TXONLY, i);
		apply_setsockopt(xsks[i]);
		if (opt_mode == MODE_RXONLY) {
			configure_fill_ring(umem);
		}
		if (opt_mode == MODE_TXONLY) {
			uint32_t len = 0;
			for (int j = 0; j < NUM_FRAMES; j++)
				gen_eth_frame(umem, j * opt_xsk_frame_size, &len);
		}
	}

	num_socks = opt_num_xsks;
	if (opt_load_xdp && opt_mode != MODE_TXONLY)
		enter_xsks_into_map();

	int cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
	for (int i = 0; i < opt_num_xsks; i++) {
		int cpu = i % cpu_count;
		args[i] = i;

		pthread_attr_init(&attrs[i]);

		if (set_affinity_attr(&attrs[i], cpu) < 0)
			exit_with_error(EINVAL);

		if ((ret = pthread_create(&threads[i], &attrs[i], run_af_xdp, &args[i])) != 0) {
			printf("Create thread for core %d\n", cpu);
			exit_with_error(ret);
		}
	}

	for (int i = 0; i < opt_num_xsks; ++i) {
		pthread_join(threads[i], NULL);
		pthread_attr_destroy(&attrs[i]);
	}

	socks_cleanup();

	free(threads);
	free(attrs);
	free(args);

	return 0;
}