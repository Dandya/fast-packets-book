
#define _GNU_SOURCE

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/ioctl.h>

// Пример реализует передачу пакетов между ядром и пользовательским пространством
// посредством общей памяти, которая представляет собой кольца RX и/или TX.
// Чтобы изучить создаваемую программой цепочку вызовов необходимо использовать
// программу strace и сделать макрос FANOUT_ENABLE равным 0, так как strace плохо работает с потоками.

// Настройки работы программы.
#define FRAME_SIZE (1 << 11) // Длина участка памяти для передачи пакета (должна быть кратна 16).
#define BLOCK_SIZE (1 << 22) // Длина участка памяти под один блок (желательно кратна FRAME_SIZE).
#define BLOCKS_COUNT 64 // Количество доступных блоков для передачи данных.
#define SEND_BUFFER 10000 // Количество пакетов, которые отправляются за один системный вызов.
#define SOCKET_MODE SOCK_RAW // Тип сокета SOCK_RAW или SOCK_DGRAM.
#define SET_PROMISC_MODE 1  // Флаг установки режима promisc.
#define FANOUT_MODE PACKET_FANOUT_CPU // Тип метода распределения пакетов по очередям.
#define FANOUT_QUEUE_COUNT 2 // Количество очередей.
#define FANOUT_ENABLE 1 // Флаг использования несколький очеречей.

bool run_flag[FANOUT_QUEUE_COUNT]; // Флаги работы потоков.
pthread_mutex_t print_mtx;         // Мьютекс для синхронизации вывода сообщений.

struct rings_buff {        // Структура с информацией о кольце.
	struct tpacket_req3 req; // Информация о кольце.
	unsigned char* mem_buff; // Указатель на общую память.
	struct iovec* rx_io;     // Блоки пакетов для захвата пакетов.
	struct iovec* tx_io;     // Блоки пакетов для отправки пакетов.
};

/*
	Структура блока для 3-й версии кольца при захвате.

	+-------------------------------+ <-- Начало блока
	| struct tpacket_block_desc     | <-- Заголовок блока
	|   uint32_t version            |
	|   uint32_t offset_to_priv     | ---+
	|   struct tpacket_hdr_v1 h1    |    |
	+-------------------------------+    |
	|           Frame 1             |    |
	|   struct tpacket3_hdr + data  |    |
	+-------------------------------+    |
	|           Frame 2             |    |
	|   struct tpacket3_hdr + data  |    |
	+-------------------------------+    |
	|              ...              |    |
	+-------------------------------+    |
	|       Приватная область       | <--+
	|   (пользовательские данные)   |
	+-------------------------------+

	Структура блока для 3-й версии кольца при отправке.

	+-------------------------------+ <-- Начало блока
	|           Frame 1             |    |
	|   struct tpacket3_hdr + data  |    |
	+-------------------------------+ <-- FRAME_SIZE
	|           Frame 2             |    |
	|   struct tpacket3_hdr + data  |    |
	+-------------------------------+ <-- FRAME_SIZE * 2
	|              ...              |    |
	+-------------------------------+ <-- FRAME_SIZE * (BLOCK_SIZE / FRAME_SIZE)
	|       Приватная область       |
	|   (пользовательские данные)   |
	+-------------------------------+
*/

struct block_desc {         // Структура для доступа к блоку пакетов.
	uint32_t version;         // Версия кольца.
	uint32_t offset_to_priv;  // Смещение до приватных данных.
	struct tpacket_hdr_v1 h1; // Заголовок с информацией о блоке.
};

struct thread_args {   // Структура с информацией пользователя.
	const char* ifname;  // Имя сетевого интерфейса.
	int mode;            // 1 - захват (receive), 0 - отправка (send).
	int fanout_group_id; // Идентификатор группы очередей пакетов.
	int fanout_id;       // Идентификатор очереди пакетов.
};

#if FANOUT_ENABLE == 1
    #define LOCK_PRINT() pthread_mutex_lock(&print_mtx)
    #define UNLOCK_PRINT() pthread_mutex_unlock(&print_mtx)
#else
    #define LOCK_PRINT()
    #define UNLOCK_PRINT()
#endif

// Обработчик сигнала.
void sigint_handler(int sig) {
	if (sig == SIGINT) {
		printf("\nStop...\n");
		for (int i = 0; i < FANOUT_QUEUE_COUNT; ++i)
			run_flag[i] = false;
	}
}

// Функция вывода Ethernet II и IPv4 заголовков.
void
print_first_34_bytes(const char* data, int len) {
	int count = len < 34 ? len : 34;
	printf("First %d bytes: ", count);
	for (int i = 0; i < count; ++i)
		printf("%02x", (unsigned char)data[i]);
	printf("\n");
}

// Функция переключения интерфейса в прослушивающий режим (promisc mode).
// Аргумент: файловый дескриптор сокета.
int
set_promisc_mode(int sock_fd, int ifindex) {
	// Заполнение аргументов для команды PACKET_ADD_MEMBERSHIP.
	// Подробнее: https://man7.org/linux/man-pages/man7/packet.7.html
	struct packet_mreq args;
	memset(&args, 0, sizeof(args));
	args.mr_type = PACKET_MR_PROMISC;
	args.mr_ifindex = ifindex;

	// Системный вызов настройки сокета.
	// Подробнее: https://man7.org/linux/man-pages/man2/setsockopt.2.html
	if (setsockopt(sock_fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &args, sizeof(args)) == -1) {
		perror("Setting in promisc mode");
		return -1;
	}

	return 0;
}

// Функция создания колец 3-й версии.
// Аргументы: файловый дескриптор сокета и указатель на структуру с информацией о кольце.
int
create_rings(int sock_fd, struct rings_buff* rings) {
	// Системный вызовы настройки сокета для установки версии колец.
	// Подробнее: https://man7.org/linux/man-pages/man7/packet.7.html
	int version = TPACKET_V3;
	if (setsockopt(sock_fd, SOL_PACKET, PACKET_VERSION, &version, sizeof(version)) == -1) {
		perror("Set rings version");
		return -1;
	}

	memset(rings, 0, sizeof(struct rings_buff));
	rings->req.tp_frame_size = FRAME_SIZE;
	rings->req.tp_block_size = BLOCK_SIZE;
	rings->req.tp_block_nr = BLOCKS_COUNT;
	rings->req.tp_frame_nr = (BLOCKS_COUNT * BLOCK_SIZE) / FRAME_SIZE;
	rings->req.tp_retire_blk_tov = 0; // Ручное освобождение блоков.
	rings->req.tp_feature_req_word = 0; // Без записи информации о хеше пакета.
	rings->req.tp_sizeof_priv = 0; // Без выделения приватной памяти в конце блока.

	// Системный вызовы настройки сокета для создания колец.
	// Подробнее: https://man7.org/linux/man-pages/man7/packet.7.html
	if (setsockopt(sock_fd, SOL_PACKET, PACKET_RX_RING, &rings->req, sizeof(rings->req)) == -1) {
		perror("Create rx ring");
		return -1;
	}
	if (setsockopt(sock_fd, SOL_PACKET, PACKET_TX_RING, &rings->req, sizeof(rings->req)) == -1) {
		perror("Create tx ring");
		return -1;
	}

	// В случае настройки захвата и отправки одновременно кольца будут.
	// расположены последовательно: сначала rx очередь, а потом tx очередь.
	// Подробнее: https://docs.kernel.org/networking/packet_mmap.html

	// Системный вызов для отображения памяти сокета в виртуальную памяти процесса.
	// Подробнее: https://man7.org/linux/man-pages/man2/mmap.2.html
	rings->mem_buff = mmap(NULL, 2 * rings->req.tp_block_size * rings->req.tp_block_nr,
			PROT_READ | PROT_WRITE, MAP_SHARED | MAP_LOCKED, sock_fd, 0);
	if (!rings->mem_buff)
		return -1;

	rings->rx_io = malloc(BLOCKS_COUNT * sizeof(struct iovec));
	if (!rings->rx_io) {
		munmap(rings->mem_buff, 2 * rings->req.tp_block_size * rings->req.tp_block_nr);
		return -1;
	}

	rings->tx_io = malloc(BLOCKS_COUNT * sizeof(struct iovec));
	if (!rings->rx_io) {
		free(rings->rx_io);
		munmap(rings->mem_buff, 2 * rings->req.tp_block_size * rings->req.tp_block_nr);
		return -1;
	}

	for (int i = 0; i < rings->req.tp_block_nr; ++i) {
		rings->rx_io[i].iov_base = rings->mem_buff + (i * rings->req.tp_block_size);
		rings->rx_io[i].iov_len = rings->req.tp_block_size;
		rings->tx_io[i].iov_base = rings->mem_buff + ((i + rings->req.tp_block_nr) * rings->req.tp_block_size);
		rings->tx_io[i].iov_len = rings->req.tp_block_size;
	}

	return 0;
}

// Функция освобождения колец 3-й версии.
// Аргумент: указатель на структуру с информацией о кольце.
void
free_rings(struct rings_buff* rings) {
	free(rings->rx_io);
	rings->rx_io = NULL;

	free(rings->tx_io);
	rings->tx_io = NULL;

	// Системный вызов для возвращения отображенной памяти.
	// Подробнее: https://man7.org/linux/man-pages/man3/munmap.3p.html
	munmap(rings->mem_buff, 2 * rings->req.tp_block_size * rings->req.tp_block_nr);
	rings->mem_buff = NULL;
}

// Функция установки источника пакетов для сокета.
// Аргументы: файловый дескриптор сокета и индекс сетевого интерфейса.
int
bind_iface_to_sock(int sock_fd, int ifindex) {
	// Заполнение аргументов для системного вызова bind.
	// ETH_P_IP --- получение пакетов IPv4.
	// Подробнее: https://man7.org/linux/man-pages/man2/bind.2.html
	struct sockaddr_ll args;
	memset(&args, 0, sizeof(args));
	args.sll_family = AF_PACKET;
	args.sll_protocol = htons(ETH_P_IP);
	args.sll_ifindex = ifindex;

	// Системный вызов настройки сокета.
	// Подробнее: https://man7.org/linux/man-pages/man2/bind.2.html
	if (bind(sock_fd, (struct sockaddr *)&args, sizeof(args)) == -1) {
		perror("Bind socket");
		return -1;
	}

	return 0;
}

#if FANOUT_ENABLE == 1
// Функция установки режима распределения пакетов по очередям.
// Аргументы: файловый дескриптор сокета и идентификатор группы очередей.
int
set_fanout(int sock_fd, int fanout_group_id) {
	int arg = (fanout_group_id | (FANOUT_MODE << 16));
	if (setsockopt(sock_fd, SOL_PACKET, PACKET_FANOUT, &arg, sizeof(arg)) == -1) {
		perror("Set fanout\n");
		return -1;
	}
	return 0;
}
#endif

// Функция открытия и настройки сокета системы AF_PACKET.
// Аргументом является название сетевого интерфейса.
// Возвращает файловый дескриптор сокета.
int
setup_af_packet(const char* ifname, int fanout_group_id, struct rings_buff* rings) {
	int sock_fd = -1;
	int ifindex = -1;

	// Системный вызов создания сокета.
	// Открываем сокет AF_PACKET, который будет получать.
	// пакеты с заголовком канального уровня или без него (SOCKET_MODE).
	// Подробнее: https://man7.org/linux/man-pages/man2/socket.2.html
	sock_fd = socket(AF_PACKET, SOCKET_MODE, 0);
	if (sock_fd == -1) {
		perror("Socket error");
		return -1;
	}

	// Получение индекса сетевого интерфейса.
	// Подробнее: https://man7.org/linux/man-pages/man3/if_indextoname.3.html
	ifindex = if_nametoindex(ifname);
	if (ifindex == 0) {
		perror("Get interface index");
		close(sock_fd);
		return -1;
	}

	// Создание колец Rx и TX.
	if (create_rings(sock_fd, rings) == -1) {
		close(sock_fd);
		return -1;
	}

	// Установка источника пакетов для сокета.
	if (bind_iface_to_sock(sock_fd, ifindex) == -1) {
		close(sock_fd);
		return -1;
	}

#if SET_PROMISC_MODE == 1
	// Перевод сетевого интерфейса в прослушивающий режим.
	if (set_promisc_mode(sock_fd, ifindex) == -1) {
		close(sock_fd);
		return -1;
	}
#endif

#if FANOUT_ENABLE == 1
	// Настройка очереди.
	set_fanout(sock_fd, fanout_group_id);
#endif

	return sock_fd;
}

// Функция установки статуса блока в доступный для записи ядром.
// Аргумент: указатель на структуру с информацией о кольце.
// Подробнее: https://www.kernel.org/doc/html/latest/networking/packet_mmap.html
void
free_block(struct block_desc* block) {
	block->h1.block_status = TP_STATUS_KERNEL;
}

// Функция чтения пакетов из блока.
// Аргумент: указатель на структуру с информацией о блоке и указатель на счётчик пакетов.
// Подробнее: https://www.kernel.org/doc/html/latest/networking/packet_mmap.html
void
read_block(struct block_desc* block, unsigned long long* pkts_count) {
	const int num_pkts = block->h1.num_pkts;
	struct tpacket3_hdr *packet = ((void *)block + block->h1.offset_to_first_pkt);
	for (int i = 0; i < num_pkts; ++i) {
		// Подробнее: linux/include/uapi/linux/if_packet.h
		LOCK_PRINT();
		printf("=== Packet ===\n");
		printf("Data length: %d bytes\n", packet->tp_len);
		printf("Time %u:%u\n", packet->tp_sec, packet->tp_nsec);
		print_first_34_bytes((void *)packet + packet->tp_mac, packet->tp_snaplen);
		UNLOCK_PRINT();
		*pkts_count += 1;
		packet = (void *)packet + packet->tp_next_offset;
	}
}

// Функция захвата пакетов.
// Аргументы: файловый дескриптор сокета, идентификатор очереди и
// указатель на структуру с информацией о кольце.
void
receive_pkts(int sock_fd, int id, struct rings_buff* rings) {
	unsigned long long pkts_count = 0;
	struct block_desc* block = NULL;
	int curr_block = 0;

	struct pollfd poll_fd;
	poll_fd.fd = sock_fd;
	poll_fd.events = POLLIN | POLLERR;
	poll_fd.revents = 0;

		LOCK_PRINT();
		printf("Receive start from ID: %d\n", id);
		UNLOCK_PRINT();

	while (run_flag[id]) {
		block = (struct block_desc*)rings->rx_io[curr_block].iov_base;
		if ((block->h1.block_status & TP_STATUS_USER) == 0) {
			// Системный вызов ожидания события.
			// Подробнее: https://man7.org/linux/man-pages/man2/poll.2.html
			poll(&poll_fd, 1, 1000);
			continue;
		}

		read_block(block, &pkts_count);
		free_block(block);
		curr_block = (curr_block + 1) % rings->req.tp_block_nr;
	}

	LOCK_PRINT();
	printf("Packets count with ID: %d:%llu\n", id, pkts_count);
	UNLOCK_PRINT();
}

// Функция отправки пакетов.
// Аргументы: файловый дескриптор сокета, идентификатор очереди и
// указатель на структуру с информацией о кольце.
void
send_pkts(int sock_fd, int id, struct rings_buff* rings) {
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

	struct pollfd poll_fd;
	poll_fd.fd = sock_fd;
	poll_fd.revents = 0;
	poll_fd.events = POLLOUT;

	const uint64_t frames_per_block = BLOCK_SIZE / FRAME_SIZE;
	const uint64_t block_count = rings->req.tp_block_nr;
	const uint64_t frame_size = rings->req.tp_frame_size;
	const uint64_t count_for_send = SEND_BUFFER;
	uint64_t pkt_i = 0;
	uint32_t block_i = 0;
	uint32_t frame_i = 0;
	struct tpacket3_hdr* packet = NULL;

	LOCK_PRINT();
	printf("Send start from ID: %d\n", id);
	UNLOCK_PRINT();

	while (run_flag[id]) {
		block_i = (pkt_i / frames_per_block) % block_count;
		frame_i = pkt_i % frames_per_block;
		packet = ((void *)rings->tx_io[block_i].iov_base + frame_i * frame_size);

		if (packet->tp_status != TP_STATUS_AVAILABLE) {
			// Системный вызов ожидания события.
			// Подробнее: https://man7.org/linux/man-pages/man2/poll.2.html
			poll(&poll_fd, 1, 1000);
			continue;
		}

		memcpy((void*)packet + sizeof(struct tpacket3_hdr), syn_pkt, sizeof(syn_pkt));
		packet->tp_len = sizeof(syn_pkt);
		packet->tp_status = TP_STATUS_SEND_REQUEST;
		if ((pkt_i % count_for_send) == 0) {
			send(sock_fd, NULL, 0, 0);
			printf("Send %lu with flag %d (ID %d)\n", count_for_send, packet->tp_status, id);
		}
		++pkt_i;
	}

	LOCK_PRINT();
	printf("Packets count with ID: %d:%lu\n", id, pkt_i);
	UNLOCK_PRINT();
}

// Функция привязки потока к логическому процессору.
// Аргументы: указатель на атрибуты потока, номер процессора.
// Подробнее: https://man7.org/linux/man-pages/man3/pthread_attr_setaffinity_np.3.html
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

// Функция запуска сокета af_packet и выполнение над ним действий.
// Аргументы: имя сетевого интерфейса, действие над сокетом, индекс группы очередей, индекс очереди.
void*
run_af_packet(void* ptr) {
	struct thread_args* args = (struct thread_args*)ptr;
	struct rings_buff rings;
	int sock_fd = setup_af_packet(args->ifname, args->fanout_group_id, &rings);
	if (sock_fd == -1)
		return NULL;

	run_flag[args->fanout_id] = true;
	if (args->mode)
		receive_pkts(sock_fd, args->fanout_id, &rings);
	else
		send_pkts(sock_fd, args->fanout_id, &rings);

	free_rings(&rings);
	close(sock_fd);
	return NULL;
}

int
main(int argc, char** argv) {
	int mode = 0;
#if FANOUT_ENABLE == 1
	int ret = 0;
	int cpu = 0;
	int cpu_count = 0;
	int fanout_group_id = 0;
	pthread_t threads[FANOUT_QUEUE_COUNT];
	pthread_attr_t attrs[FANOUT_QUEUE_COUNT];
	struct thread_args args[FANOUT_QUEUE_COUNT];
#else
	struct thread_args args;
#endif

	if (argc != 3) {
		printf("Usage: %s INTERFACE MODE\n", argv[0]);
		printf("INTERFACE: name of network device\n");
		printf("MODE: strings `RECEIVE` or `SEND`\n");
		return 1;
	}

	if (strcmp(argv[2], "RECEIVE") == 0) {
		mode = 1;
	} else if (strcmp(argv[2], "SEND") == 0) {
		mode = 0;
	} else {
		printf("Unknown mode\n");
		return 1;
	}

#if FANOUT_ENABLE == 1
	pthread_mutex_init(&print_mtx, NULL);
#endif

	if (signal(SIGINT, sigint_handler) == SIG_ERR) {
		perror("signal");
		return 2;
	}

#if FANOUT_ENABLE == 0
	args.ifname = argv[1];
	args.mode = mode;
	args.fanout_group_id = 0;
	args.fanout_id = 0;
	run_af_packet(&args);
#else
	cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
	fanout_group_id = getpid() & 0xffff;
	printf("Count of CPU: %d\n", cpu_count);
	printf("Fanout group ID: %d\n", fanout_group_id);

	for (int i = 0; i < FANOUT_QUEUE_COUNT; ++i) {
		pthread_attr_init(&attrs[i]);

		args[i].ifname = argv[1];
		args[i].mode = mode;
		args[i].fanout_group_id = fanout_group_id;
		args[i].fanout_id =  i;
		cpu = i % cpu_count;

		if (set_affinity_attr(&attrs[i], cpu) < 0)
			exit(3);

		if ((ret = pthread_create(&threads[i], &attrs[i], run_af_packet, &args[i])) != 0) {
			printf("Create thread for core %d: %s\n", cpu, strerror(ret));
			exit(3);
		}
	}

	for (int i = 0; i < FANOUT_QUEUE_COUNT; ++i) {
		pthread_join(threads[i], NULL);
		pthread_attr_destroy(&attrs[i]);
	}
#endif

	return 0;
}

