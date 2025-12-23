
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>

// Основан на примере:
//   https://github.com/DPDK/dpdk/tree/main/examples/skeleton

// TODO векторы, много поточность

#define DEBUG_HEXDUMP 1

#define RX_RING_SIZE 1024
#define TX_RING_SIZE 1024

#define NUM_MBUFS 8191
#define MBUF_CACHE_SIZE 250
#define BURST_SIZE 1

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

static bool work_done = false;

// Функция вывода пакета в hex виде.
static void
hex_dump(struct rte_mbuf* buff) {
	if (!DEBUG_HEXDUMP)
		return;

	struct rte_mbuf* seg;
	uint32_t offset = 0;
	uint32_t pkt_len = rte_pktmbuf_pkt_len(buff);
	size_t line_size = 32;
	uint32_t bytes_processed = 0;

	printf("======================================\n");
	printf("length = %u\n", pkt_len);
	printf("segments count = %u\n", buff->nb_segs);

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
}

// Инициализация сетевого интерфейса.
// Аргументы: индекс сетевого интерфейса и указатель на кольцо пакетов.
static inline int
port_init(uint16_t port, struct rte_mempool *mbuf_pool) {
	struct rte_eth_conf port_conf;
	const uint16_t rx_rings = 1, tx_rings = 1;
	uint16_t nb_rxd = RX_RING_SIZE;
	uint16_t nb_txd = TX_RING_SIZE;
	int retval;
	uint16_t q;
	struct rte_eth_dev_info dev_info;
	struct rte_eth_txconf txconf;

	// Проверка на валидность индекса сетевого интерфейса.
	if (!rte_eth_dev_is_valid_port(port))
		return -1;

	memset(&port_conf, 0, sizeof(struct rte_eth_conf));

	// Получение информации о сетевом интерфейсе.
	retval = rte_eth_dev_info_get(port, &dev_info);
	if (retval != 0) {
		printf("Error during getting device (port %u) info: %s\n",
				port, strerror(-retval));
		return retval;
	}

	// Установка гарантии, что все отправляемые пакеты принадлежат одному кольцу.
	if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE)
		port_conf.txmode.offloads |=
			RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;

	// Настройка сетевого интерфейса согласно параметрам.
	retval = rte_eth_dev_configure(port, rx_rings, tx_rings, &port_conf);
	if (retval != 0)
		return retval;

	// Установка размера колец RX и TX.
	retval = rte_eth_dev_adjust_nb_rx_tx_desc(port, &nb_rxd, &nb_txd);
	if (retval != 0)
		return retval;

	for (q = 0; q < rx_rings; q++) {
		// Установка памяти для очереди RX.
		retval = rte_eth_rx_queue_setup(port, q, nb_rxd,
				rte_eth_dev_socket_id(port), NULL, mbuf_pool);
		if (retval < 0)
			return retval;
	}

	txconf = dev_info.default_txconf;
	txconf.offloads = port_conf.txmode.offloads;
	for (q = 0; q < tx_rings; q++) {
		// Установка памяти для очереди TX.
		retval = rte_eth_tx_queue_setup(port, q, nb_txd,
				rte_eth_dev_socket_id(port), &txconf);
		if (retval < 0)
			return retval;
	}

	// Запуск сетевого интерфейса.
	retval = rte_eth_dev_start(port);
	if (retval < 0)
		return retval;

	// Переключение интерфейса в режим "прослушивания".
	retval = rte_eth_promiscuous_enable(port);
	if (retval != 0)
		return retval;

	return 0;
}

static void
lcore_main(struct rte_mempool *mbuf_pool, uint16_t port, int mode, uint64_t* count) {
	printf("\nCore %u run.\n", rte_lcore_id());

	if (mode == 0) {
		while (!work_done) {
			struct rte_mbuf *bufs[BURST_SIZE];
			const uint16_t nb_rx = rte_eth_rx_burst(port, 0, bufs, BURST_SIZE);

			if (unlikely(nb_rx == 0)) {
				rte_pause();
				continue;
			}

			for (uint16_t i = 0; i < nb_rx; ++i) {
				hex_dump(bufs[i]);
				rte_pktmbuf_free(bufs[i]);
			}

			*count += nb_rx;
		}
	} else {
		while (!work_done) {
			struct rte_mbuf *bufs;
			if (rte_pktmbuf_alloc_bulk(mbuf_pool, &bufs, BURST_SIZE) < 0) {
				printf("Error: failed to allocate mbuf\n");
				continue;
			}

			int i = 0;
			for (; i < BURST_SIZE; ++i) {
				if (unlikely(rte_pktmbuf_tailroom(&bufs[i]) < sizeof(syn_pkt))) {
        	printf("Error: not enough tailroom in mbuf %d\n", i);
        	rte_pktmbuf_free_bulk(&bufs, BURST_SIZE);
        	break;
				}

				char *data = rte_pktmbuf_append(&bufs[i], sizeof(syn_pkt));
				if (unlikely(data == NULL)) {
					rte_pktmbuf_free_bulk(&bufs, BURST_SIZE);
					printf("Error: failed to append data to mbuf %d\n", i);
					break;
				}

				rte_memcpy(data, syn_pkt, sizeof(syn_pkt));
			}

			if (i == BURST_SIZE) {
				const uint16_t nb_tx = rte_eth_tx_burst(port, 0, &bufs, BURST_SIZE);
				rte_pktmbuf_free_bulk(&bufs, BURST_SIZE);
				*count += nb_tx;
			}
		}
	}
}

static void
signal_handler(int signum) {
	if (signum == SIGINT) {
		printf("\n\nSignal %d received, preparing to exit...\n", signum);
		work_done = true;
	}
}

int
main(int argc, char *argv[]) {
	struct rte_mempool *mbuf_pool;
	unsigned nb_ports;
	uint16_t portid;
	int mode = -1;

	// Инициализация подсистемы EAL (Environment Abstraction Layer).
	int ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Error: EAL initialization failed\n");

	// Обработка параметров.
	argc -= ret;
	argv += ret;

	portid = (uint16_t)atoi(argv[1]);
	if (strncmp(argv[2], "rx", 2) == 0)
		mode = 0;
	else if (strncmp(argv[2], "tx", 2) == 0)
		mode = 1;
	else
		rte_exit(EXIT_FAILURE, "Error: unknown mode\n");

	signal(SIGINT, signal_handler);

	// Получение количества доступных сетевых интерфейсов.
	nb_ports = rte_eth_dev_count_avail();
	if (portid >= nb_ports)
		rte_exit(EXIT_FAILURE, "Error: unknown network port\n");

	// Создание именованного кольца памяти.
	mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS,
		MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());

	if (mbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Error: cannot create mbuf pool\n");

	// Инициализация сетевого интерфейса.
	if (port_init(portid, mbuf_pool) != 0)
		rte_exit(EXIT_FAILURE, "Error: сannot init port %"PRIu16 "\n", portid);

	// Запуск потока.
	uint64_t count = 0;
	lcore_main(mbuf_pool, portid, mode, &count);
	printf("Count = %llu\n", count);

	// Очистка подсистемы EAL.
	rte_eal_cleanup();

	return 0;
}