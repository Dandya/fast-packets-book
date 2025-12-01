
#define _GNU_SOURCE

#include <errno.h>
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
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/ioctl.h>

// Пример реализует передачу пакетов между ядром и пользовательским пространством
// посредством системных вызовов. Чтобы изучить создаваемую программой цепочку вызовов
// необходимо использовать программу strace и сделать макрос FANOUT_ENABLE равным 0,
// так как strace плохо работает с потоками.

// Настройки работы программы.
#define CONTROL_LEN 128 // Длина буфера для передачи времени.
#define BUFFER_LEN 1500 // Длина буфера для передачи пакетов.
#define SOCKET_MODE SOCK_RAW // Тип сокета SOCK_RAW или SOCK_DGRAM.
#define SET_PROMISC_MODE 1 // Флаг установки режима promisc.
#define FANOUT_MODE PACKET_FANOUT_CPU // Тип метода распределения пакетов по очередям.
#define FANOUT_QUEUE_COUNT 2 // Количество очередей.
#define FANOUT_ENABLE 1 // Флаг использования несколький очеречей.

bool run_flag[FANOUT_QUEUE_COUNT]; // Флаги работы потоков.
pthread_mutex_t print_mtx;         // Мьютекс для синхронизации вывода сообщений.

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

#ifndef __STRICT_ANSI__
// Функция включения сохранения времени захвата пакетов.
// Аргумент: файловый дескриптор сокета.
int
set_timestamps(int sock_fd) {
	int flag = 1;

	// Системный вызов настройки сокета.
	// Подробнее: https://man7.org/linux/man-pages/man2/setsockopt.2.html
	if (setsockopt(sock_fd, SOL_SOCKET, SO_TIMESTAMP, &flag, sizeof(flag)) < 0) {
		perror("Set timestamps");
		return -1;
	}
	return 0;
}
#endif

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

// Функция установки времени ожидания для функици чтения из сокета.
// Аргумент: файловый дескриптор сокета.
int
set_recv_timeout(int sock_fd) {
	struct timeval tv = {1, 0};

	if (setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
		perror("Set timeout");
		return -1;
	}
	return 0;
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
setup_af_packet(const char* ifname, int fanout_group_id) {
	int sock_fd = -1;
	int ifindex = -1;

	// Системный вызов создания сокета.
	// Открываем сокет AF_PACKET, который будет получать
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

#ifndef __STRICT_ANSI__
	// Установка записи времени.
	if (set_timestamps(sock_fd) == -1) {
		close(sock_fd);
		return -1;
	}
#endif

	// Установка времени ожидания для чтения.
	if (set_recv_timeout(sock_fd) < 0) {
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

// Функция захвата пакетов.
// Аргументы: файловый дескриптор сокета и идентификатор очереди.
void
receive_pkts(int sock_fd, int id) {
	unsigned long long pkts_count = 0;

	LOCK_PRINT();
	printf("Receive start from ID: %d\n", id);
	UNLOCK_PRINT();

#ifndef __STRICT_ANSI__
	// Заполнение структур для получения времени захвата.
	// Подробнее: https://man7.org/linux/man-pages/man3/cmsg.3.html
	//            https://man7.org/linux/man-pages/man7/socket.7.html
	char msg_buff[BUFFER_LEN];
	char ctrl_buff[CONTROL_LEN];
	struct timeval tv;
	struct cmsghdr *cmsg;
	struct iovec iov = {
		.iov_base = msg_buff,
		.iov_len = sizeof(msg_buff)
	};
	struct msghdr msg = {
		.msg_name = NULL,
		.msg_namelen = 0,
		.msg_iov = &iov,
		.msg_iovlen = 1,
		.msg_control = ctrl_buff,
		.msg_controllen = sizeof(ctrl_buff),
		.msg_flags = 0
	};

	while (run_flag[id]) {
		// Системный вызов для чтения данных из сокета.
		// Подробнее: https://man7.org/linux/man-pages/man3/recvmsg.3p.html
		int len = recvmsg(sock_fd, &msg, 0);
		if (len < 0) {
			if (errno == EINTR || errno == EAGAIN) // Ожидание прервано.
				continue;
			perror("Receive fail");
			return;
		}

		// Перебор доступных заголовков Command Message.
		// Подробнее: https://man7.org/linux/man-pages/man3/cmsg.3.html
		for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
			if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_TIMESTAMP) {
				memcpy(&tv, CMSG_DATA(cmsg), sizeof(tv));
				break;
			}
		}

		LOCK_PRINT();
		printf("=== Packet ===\n");
		printf("Data length: %d bytes\n", len);
		printf("Time %ld:%ld\n", tv.tv_sec, tv.tv_usec);
		print_first_34_bytes(iov.iov_base, len);
		UNLOCK_PRINT();
		++pkts_count;
	}
#else
	char buffer[BUFFER_LEN];

	while (run_flag[id]) {
		// Системный вызов чтения данных из сокета.
		// Подробнее: https://man7.org/linux/man-pages/man2/read.2.html
		int len = read(sock_fd, buffer, BUFFER_LEN);
		if (len < 0) {
			if (errno == EINTR || errno == EAGAIN) // Ожидание прервано.
				continue;
			perror("Receive fail");
			return;
		}

		LOCK_PRINT();
		printf("Received packet with len %d\n", len);
		print_first_34_bytes(buffer, len);
		UNLOCK_PRINT();
		++pkts_count;
	}
#endif // __STRICT_ANSI__

	LOCK_PRINT();
	printf("Packets count with ID: %d:%llu\n", id, pkts_count);
	UNLOCK_PRINT();
}

// Функция отправки пакетов.
// Аргументы: файловый дескриптор сокета и идентификатор очереди.
void
send_pkts(int sock_fd, int id) {
	unsigned long long pkts_count = 0;
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

		LOCK_PRINT();
		printf("Send start from ID: %d\n", id);
		UNLOCK_PRINT();

	while (run_flag[id]) {
		// Системный вызов записи данных в сокет.
		// Подробнее: https://man7.org/linux/man-pages/man2/write.2.html
		int len = write(sock_fd, syn_pkt, sizeof(syn_pkt));
		if (len < 0) {
			perror("Send fail");
			return;
		}
		++pkts_count;
	}

	LOCK_PRINT();
	printf("Packets count with ID: %d:%llu\n", id, pkts_count);
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
	int sock_fd = setup_af_packet(args->ifname, args->fanout_group_id);
	if (sock_fd == -1)
		return NULL;

	run_flag[args->fanout_id] = true;
	if (args->mode)
		receive_pkts(sock_fd, args->fanout_id);
	else
		send_pkts(sock_fd, args->fanout_id);

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

