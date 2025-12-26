
#include <inttypes.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>

// Основан на примере:
//   https://github.com/DPDK/dpdk/tree/main/examples/skeleton

// Параметр вывода данных о пакете.
#define DEBUG_HEXDUMP 1

// Количество дескрипторов в кольцах RX и TX.
#define RX_RING_SIZE 1024
#define TX_RING_SIZE 1024

// Параметры памяти `rte_mempool`.
#define NUM_MBUFS 8191
#define MBUF_CACHE_SIZE 250

// Количество пакетов обрабатываемых на одно действие чтения или отправки.
#define BURST_SIZE 256

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

// Флаг окончания работы.
static bool work_done = false;
// Мьютекс для синхронизации вывода текста в терминал.
static pthread_mutex_t mutex;
// Частота процессора.
uint64_t tsc_hz;
// Количество тиков процессора с начала работы программы.
uint64_t tsc_start;
// Индекс сетевого интерфейса (порта).
static uint16_t opt_port_id = 0;
// Режим работы программы (захват или отправка).
static int opt_mode = 0;
// Количество очередей (колец) приема или отправки пакетов.
static uint16_t opt_queue_count = 1;
// Количество пакетов для отправки.
static uint64_t opt_tx_count = INT64_MAX;

// Функция получения тиков процессора.
static inline uint64_t
get_current_tsc(void) {
  return rte_rdtsc();
}

// Функция вывода пакета в hex виде.
static void
hex_dump(struct rte_mbuf* buff, int id) {
	if (!DEBUG_HEXDUMP)
		return;

	struct rte_mbuf* seg;
	uint32_t offset = 0;
	uint32_t pkt_len = rte_pktmbuf_pkt_len(buff);
	size_t line_size = 32;
	uint32_t bytes_processed = 0;

	uint64_t rx_time = get_current_tsc();
	uint64_t sec = (rx_time - tsc_start)/ tsc_hz;

	pthread_mutex_lock(&mutex);
	printf("======================================\n");
	printf("id = %u\n", id);
	printf("length = %u\n", pkt_len);
	printf("segments count = %u\n", buff->nb_segs);
	printf("seconds from start = %llu\n", sec);

	// Итерация по всем сегментам.
	for (seg = buff; seg != NULL; seg = seg->next) {
		uint16_t seg_len = rte_pktmbuf_data_len(seg);
		uint8_t *seg_data = rte_pktmbuf_mtod(seg, uint8_t*);

		printf("+----- segment start -----+\n");

		for (uint16_t i = 0; i < seg_len && bytes_processed < pkt_len; i++) {
			if (bytes_processed % 16 == 0) {
				if (bytes_processed > 0)
					printf("\n");
				printf("%04X: ", bytes_processed);
			} else if (bytes_processed % 8 == 0) {
				printf(" ");
			}

			printf("%02X ", seg_data[i]);
			bytes_processed++;
		}
	}
	printf("\n");
	pthread_mutex_unlock(&mutex);
}

// Инициализация сетевого интерфейса (порта).
// Аргументы: индекс сетевого интерфейса и указатель на кольцо пакетов.
static inline int
port_init(uint16_t port, struct rte_mempool *mbuf_pool) {
	const uint16_t rx_queue_count = opt_queue_count
	const uint16_t tx_queue_count = opt_queue_count;
	uint16_t nb_rxd = RX_RING_SIZE;
	uint16_t nb_txd = TX_RING_SIZE;
	int retval;
	uint16_t q;
	struct rte_eth_dev_info dev_info;
	struct rte_eth_txconf txconf;

	// Проверка на валидность индекса сетевого интерфейса.
	// Подробнее: https://doc.dpdk.org/api/rte__ethdev_8h.html#a22dcfd3f5f2b34f657131d66132e23a7
	if (!rte_eth_dev_is_valid_port(port))
		return -1;

	struct rte_eth_conf port_conf = {
		.rxmode = {
			.mtu = 1500, // Величена MTU
			.mq_mode = RTE_ETH_MQ_RX_RSS, // Включение распределения пакетов по очередям.
		},
		.rx_adv_conf = {
			.rss_conf = { // Правила распределения пакетов по очередям
				.rss_key = NULL, // Распределение пакетов на основе IPv4, TCP и UDP.
				.rss_hf = RTE_ETH_RSS_NONFRAG_IPV4_TCP | RTE_ETH_RSS_NONFRAG_IPV4_UDP,
			},
		}
	};

	// Получение информации о сетевом интерфейсе.
	// Подробнее: https://doc.dpdk.org/api/rte__ethdev_8h.html#a47933dd514cda48f158117ddfa139658
	retval = rte_eth_dev_info_get(port, &dev_info);
	if (retval != 0) {
		printf("Error during getting device (port %u) info: %s\n", port, strerror(-retval));
		return retval;
	}

	// Установка гарантии, что все отправляемые пакеты принадлежат одному кольцу.
	if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE)
		port_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;

	// Настройка колец `RX` и `TX` сетевого интерфейса согласно параметрам.
	// Подробнее: https://doc.dpdk.org/api/rte__ethdev_8h.html#a1a7d3a20b102fee222541fda50fd87bd
	retval = rte_eth_dev_configure(port, rx_queue_count, tx_queue_count, &port_conf);
	if (retval != 0)
		return retval;

	// Проверка и установка размера колец RX и TX.
	// Подробнее: https://doc.dpdk.org/api/rte__ethdev_8h.html#ad31219b87a1733d5b367a7c04c7f7b48
	retval = rte_eth_dev_adjust_nb_rx_tx_desc(port, &nb_rxd, &nb_txd);
	if (retval != 0)
		return retval;

	for (q = 0; q < rx_queue_count; q++) {
		// Выделение памяти для кольца RX и ей настройка.
		// Подробнее: https://doc.dpdk.org/api/rte__ethdev_8h.html#a36ba70a5a6fce2c2c1f774828ba78f8d
		retval = rte_eth_rx_queue_setup(port, q, nb_rxd, rte_eth_dev_socket_id(port), NULL, mbuf_pool);
		if (retval < 0)
			return retval;
	}

	txconf = dev_info.default_txconf;
	txconf.offloads = port_conf.txmode.offloads;
	for (q = 0; q < tx_queue_count; q++) {
		// Выделение памяти для кольца TX и ей настройка.
		// Подробнее: https://doc.dpdk.org/api/rte__ethdev_8h.html#a796c2f20778984c6f41b271e36bae50e
		retval = rte_eth_tx_queue_setup(port, q, nb_txd, rte_eth_dev_socket_id(port), &txconf);
		if (retval < 0)
			return retval;
	}

	// Запуск сетевого интерфейса.
	// Подробнее: https://doc.dpdk.org/api/rte__ethdev_8h.html#afdc834c1c52e9fb512301990468ca7c2
	retval = rte_eth_dev_start(port);
	if (retval < 0)
		return retval;

	// Переключение интерфейса в режим "прослушивания".
	// Подробнее: https://doc.dpdk.org/api/rte__ethdev_8h.html#a5dd1dedaa45f05c72bcc35495e441e91
	retval = rte_eth_promiscuous_enable(port);
	if (retval != 0)
		return retval;

	return 0;
}

// Структура данных для потока захвата/отправки пакетов.
struct thread_args {
	struct rte_mempool* mbuf_pool;
	uint16_t port;
	uint16_t queue;
	int mode;
	uint64_t* count;
};

// Функция захвата или отправки пакетов.
static int
lcore_main(void* arg) {
	struct thread_args* args = (struct thread_args*)arg;

	if (args->mode == 0) {
		while (!work_done) {
			struct rte_mbuf *bufs[BURST_SIZE];
			// Получения аллоцированных пакетов.
			// Подробнее: https://doc.dpdk.org/api/rte__ethdev_8h.html#a3e7d76a451b46348686ea97d6367f102
			const uint16_t nb_rx = rte_eth_rx_burst(args->port, args->queue, bufs, BURST_SIZE);

			if (unlikely(nb_rx == 0)) {
				// Небольшая задержка для снижения энергопотребления.
				// Подробнее: https://doc.dpdk.org/api/rte__pause_8h.html#ad59aa7777c93d3cfd5f10617a3acd1c5
				rte_pause();
				continue;
			}

			for (uint16_t i = 0; i < nb_rx; ++i) {
				hex_dump(bufs[i], args->queue);
				// Возвращение буфера в память `rte_mempool`.
				// Подробнее: https://doc.dpdk.org/api/rte__mbuf_8h.html#a1215458932900b7cd5192326fa4a6902
				rte_pktmbuf_free(bufs[i]);
			}

			*args->count += nb_rx;
		}
	} else {
		while (!work_done && *args->count < opt_tx_count) {
			struct rte_mbuf *bufs[BURST_SIZE];
			// Аллоцирование нескольких пакетов (взятие из `rte_mempool`).
			// Подробнее: https://doc.dpdk.org/api/rte__mbuf_8h.html#ae3d2aeb7f1189a3a6c33c861391cb16b
			if (rte_pktmbuf_alloc_bulk(args->mbuf_pool, bufs, BURST_SIZE) < 0) {
				printf("Error: failed to allocate mbuf\n");
				continue;
			}

			int i = 0;
			for (; i < BURST_SIZE; ++i) {
				// Получение свободного места в буфере.
				// Подробнее: https://doc.dpdk.org/api/rte__mbuf_8h.html#a257bc8af3e8fde7eb6603bdf4ae0528e
				if (unlikely(rte_pktmbuf_tailroom(bufs[i]) < sizeof(syn_pkt))) {
        	printf("Error: not enough tailroom in mbuf %d\n", i);
					rte_pktmbuf_free_bulk(bufs, BURST_SIZE);
        	break;
				}

				// Настройка буфера до опреленной длины и получение данных.
				// Подробнее: https://doc.dpdk.org/api/rte__mbuf_8h.html#a603c04217c8dd3e35c45e71b15cf11f4
				char *data = rte_pktmbuf_append(bufs[i], sizeof(syn_pkt));
				if (unlikely(data == NULL)) {
					rte_pktmbuf_free_bulk(bufs, BURST_SIZE);
					printf("Error: failed to append data to mbuf %d\n", i);
					break;
				}

				// Аналог memcpy для копирования данных.
				rte_memcpy(data, syn_pkt, sizeof(syn_pkt));
			}

			if (i == BURST_SIZE) {
				// Отправка нескольких пакетов и их последующие освобожнение в `rte_mempool`.
				// Подробнее: https://doc.dpdk.org/api/rte__ethdev_8h.html#a83e56cabbd31637efd648e3fc010392b
				const uint16_t nb_tx = rte_eth_tx_burst(args->port, args->queue, bufs,
						((opt_tx_count - *args->count) > BURST_SIZE ? BURST_SIZE : opt_tx_count - *args->count));
				// Освобождение в `rte_mempool` неотправленных пакетов.
				// Подробнее: https://doc.dpdk.org/api/rte__mbuf_8h.html#a90e7796f902bcaa856e274a30c68e47f
				rte_pktmbuf_free_bulk(bufs, BURST_SIZE);
				*args->count += nb_tx;
			}
		}
	}
	return 0;
}

// Обработка сигнала SIGINT.
static void
signal_handler(int signum) {
	if (signum == SIGINT) {
		printf("\n\nSignal %d received, preparing to exit...\n", signum);
		work_done = true;
	}
}

static struct option long_options[] = {
	{"rxonly", no_argument, 0, 'r'},
	{"txonly", no_argument, 0, 't'},
	{"port", required_argument, 0, 'p'},
	{"queue_count", required_argument, 0, 'q'},
	{"tx-pkt-count", required_argument, 0, 'C'},
	{"help", no_argument, 0, 'h'},
	{0, 0, 0, 0}
};

static void
usage(const char* prog) {
	const char* str =
		"  Usage: %s [OPTIONS]\n"
		"  Options:\n"
		"  -r, --rxonly		Print all incoming packets (default).\n"
		"  -t, --txonly		Only send packets.\n"
		"  -p, --port=<INDEX>	Run on port.\n"
		"  -q, --queues=n	Use n queues (default 1).\n"
		"  -C, --tx-pkt-count=n	Number of packets to send.\n"
		"			Default: Continuous packets.\n"
		"  -h, --help	Print this help.\n"
		"\n";
	fprintf(stderr, str, prog);

	exit(EXIT_FAILURE);
}

// Функция парсинга аргументов командной строки.
static void
parse_command_line(int argc, char** argv) {
	int option_index, c;
	opterr = 0;

	for (;;) {
		c = getopt_long(argc, argv, "rtp:q:C:h", long_options, &option_index);
		if (c == -1)
			break;

		switch (c) {
		case 'r':
			opt_mode = 0;
			break;
		case 't':
			opt_mode = 1;
			break;
		case 'p':
			opt_port_id = (uint16_t)atoi(optarg);
			break;
		case 'q':
			opt_queue_count = (uint16_t)atoi(optarg);
			break;
		case 'C':
			opt_tx_count = (uint64_t)atoll(optarg);
			break;
		default:
			usage(argv[0]);
		}
	}
}

int
main(int argc, char *argv[]) {
	struct rte_mempool *mbuf_pool;
	unsigned nb_ports;

	pthread_mutex_init(&mutex, NULL);

	// Инициализация подсистемы EAL (Environment Abstraction Layer).
	// Подробнее: https://doc.dpdk.org/api/rte__eal_8h.html#a5c3f4dddc25e38c5a186ecd8a69260e3
	int ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Error: EAL initialization failed\n");

	// Обработка параметров.
	argc -= ret;
	argv += ret;

	parse_command_line(argc, argv);

	signal(SIGINT, signal_handler);

	tsc_hz = rte_get_tsc_hz();
	tsc_start = get_current_tsc();

	// Получение количества доступных сетевых интерфейсов.
	// Подробнее: https://doc.dpdk.org/api/rte__ethdev_8h.html#a9ab708089665bebcd65cefe5383b24f2
	nb_ports = rte_eth_dev_count_avail();
	if (opt_port_id >= nb_ports)
		rte_exit(EXIT_FAILURE, "Error: unknown network port\n");

	// Создание именованного кольца памяти.
	// Подробнее: https://doc.dpdk.org/api/rte__mbuf_8h.html#a8f4abb0d54753d2fde515f35c1ba402a
	mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS,
		MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());

	if (mbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Error: cannot create mbuf pool\n");

	// Инициализация сетевого интерфейса.
	if (port_init(opt_port_id, mbuf_pool) != 0)
		rte_exit(EXIT_FAILURE, "Error: сannot init port %"PRIu16 "\n", opt_port_id);

	unsigned int lcore_id;
	int queue_id = 0;
	struct thread_args* args = calloc(opt_queue_count, sizeof(struct thread_args));
	uint64_t* counts = calloc(opt_queue_count, sizeof(uint64_t));

	// Цикл по доступным «дополнительным» ядрам.
	// Функция main исполняется на отдельном ядре.
	RTE_LCORE_FOREACH_WORKER(lcore_id) {
		if (queue_id >= opt_queue_count) break;

		args[queue_id].mbuf_pool = mbuf_pool;
		args[queue_id].port = opt_port_id;
		args[queue_id].queue = queue_id;
		args[queue_id].mode = opt_mode;
		args[queue_id].count = &counts[queue_id];
		counts[queue_id] = 0;

		// Запуск отдельного потока исполнения на ядре `lcore_id`.
		// Подробнее: https://doc.dpdk.org/api/rte__launch_8h.html#a2bf98eda211728b3dc69aa7694758c6d
		rte_eal_remote_launch((lcore_function_t *)lcore_main, &args[queue_id], lcore_id);

		queue_id++;
	}

	printf("Started %d threads\n", queue_id);

	RTE_LCORE_FOREACH_WORKER(lcore_id) {
		if (lcore_id < queue_id)
			// Ожидание завершения потоков на «дополнительных» ядрах.
			// Подробнее: https://doc.dpdk.org/api/rte__launch_8h.html#ae9500e1d35bd4cfb95d18c0be863cb1e
			rte_eal_wait_lcore(lcore_id);
	}

	// Очистка подсистемы EAL.
	// Подробнее: https://doc.dpdk.org/api/rte__eal_8h.html#a7a745887f62a82dc83f1524e2ff2a236
	rte_eal_cleanup();

	uint64_t all_count = 0;
	for (int i = 0; i < opt_queue_count; ++i) {
		all_count += counts[i];
		printf("Thread %d: %llu\n", i, counts[i]);
	}
	printf("All: %llu\n", all_count);

	free(args);
	free(counts);

	pthread_mutex_destroy(&mutex);

	return 0;
}