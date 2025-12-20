# Анализ системы AF_PACKET

***

В данном разделе разбирается устройство системы AF_PACKET, а также примеры её использования.

***

1. [Описание архитектуры](#описание-архитектуры)
   1. [Применяемые механизмы и технологии](#применяемые-механизмы-и-технологии)
   2. [Доступные режимы](#доступные-режимы)
   3. [Процесс передачи сетевых пакетов](#процесс-передачи-сетевых-пакетов)
2. [Разбор примеров](#разбор-примеров)
   1. [Настройка сокета](#настройка-сокета)
   2. [Захват и отправка сетевых пакетов](#захват-и-отправка-сетевых-пакетов)
3. [Заключение](#заключение)
4. [Полезные материалы](#полезные-материалы)
5. [Источники](#источники)

***

## Описание архитектуры

### Применяемые механизмы и технологии

Система «AF_PACKET» основывается на интерфейсе сокетов ядра «Linux». Полученные пакеты, хранящиеся в структурах `sk_buff` передаются в «AF_PACKET», откуда посредством отображения памяти через `mmap` или с помощью системных вызовов `read/write` передаются в пользовательское пространство [1].

### Доступные режимы

Передача пакетов доступна в системе «AF_PACKET» в двух видах:

1. `SOCK_RAW` — пакеты с заголовком канального уровня;
2. `SOCK_DGRAM` — пакеты без заголовка канального уровня.

Для получения или отправки пакетов используются системные вызовы `recvfrom/sendto`, что сильно нагружает систему. Поэтому с версии ядра 2.4 появилась возможность использования общей памяти между ядром и пользовательским пространством, что сильно уменьшает накладные расходы на передачу пакетов между пространствами. Этот механизм получил название «PACKET_MMAP» и представляет собой кольцевой буфер сетевых пакетов.

Чтобы перейти от системных вызовов к общей памяти, необходимо с помощью системного вызова `setsockopt`  передать параметры кольцевого буфера командой `PACKET_RX_RING`/`PACKET_TX_RING` на уровне `SOL_PACKET`. По умолчанию создаётся кольцо первой версии `TPACKET_V1`, но чтобы создать кольцо более современной версии необходимо также с помощью системного вызова `setsockopt` передать нужную версию командой `PACKET_VERSION` на уровне `SOL_PACKET` [2].

Рассмотрим различия всех трёх версий работы механизма «PACKET_MMAP»:

1. В версии `TPACKET_V1`:
	 - Доступны кольца RX и TX;
	 - Доступно время получения пакета с точностью до миллисекунд;
	 - Метаданные пакета доступны через структуру `tpacket_hdr`.
2. В версии `TPACKET_V2`:
	 - Доступно время получения пакета с точностью до наносекунд;
	 - Доступна информация о VLAN;
	 - Метаданные пакета доступны через структуру `tpacket2_hdr`.
3. В версии `TPACKET_V3`:
	 - Передача сетевых пакетов блоками (наборами);
	 - Доступна динамическая настройка размера блока;
	 - Доступна настройка времени ожидания новых пакетов;
	 - Доступна информация о VLAN;
	 - Метаданные пакета доступны через структуру `tpacket3_hdr`.

Также с версии ядра «Linux» 3.1 стала доступна команда `PACKET_FANOUT`, которая позволяет настроить балансировку нагрузки, распределяя полученные пакеты по очередям и избегая синхронизации данных между процессорами, потоками. Доступны следующие значения:

1. `PACKET_FANOUT_HASH` — распределение пакетов на основе хеша из адресов, портов и номера протокола транспортного уровня;
2. `PACKET_FANOUT_LB` — циклическое распределение пакетов по очередям;
3. `PACKET_FANOUT_CPU` — привязка к номеру логического процессора, что позволяет избежать блокировок для синхронизации данных;
4. `PACKET_FANOUT_RND` — случайное распределение по очередям;
5. `PACKET_FANOUT_ROLLOVER` — заполнение очередей в зависимости от их заполненности;
6. `PACKET_FANOUT_QM` — привязка к очередям сетевой карты;
7. `PACKET_FANOUT_CBPF` — распределение на основе классического BPF (см. [фильтрация пакетов](BPF.md));
8. `PACKET_FANOUT_EBPF` — распределение на основе расширенного BPF (см. [фильтрация пакетов](BPF.md));
9. `PACKET_FANOUT_FLAG_DEFRAG` — дополнительный параметр, который включает дефрагментацию пакетов IP до передачи их в сокет;
10. `PACKET_FANOUT_FLAG_ROLLOVER` — дополнительный параметр, который распределяет пакет в следующую очередь, если нужная заполнена.

### Процесс передачи сетевых пакетов

Для изучения процесса передачи сетевых пакетов из пространства ядра «Linux» в пользовательское пространство иследует обратится к исходному коду системы в файле `contrib/linux-6.18/net/packet/af_packet.c`.

Так как передача пакетов возможна в системе «AF_PACKET» с помощью двух способов, рассмотрим сначала передачу через системные вызовы для тип сокета `SOCK_RAW`. Чтобы сокет мог обработать системный вызов, модулю ядра необходимо зарегистрировать соответствующие callback-функции в структуре `proto_ops` и передать сетевому стеку через структуру `sock` при создании сокета.

```c
// contrib/linux-6.18/net/packet/af_packet.c
// Пример структур с callback-функциями для работы af_packet.
static const struct proto_ops packet_ops_spkt = { // SOCK_DGRAM
	.family =	PF_PACKET,
	.owner =	THIS_MODULE,
	.release =	packet_release,
	.bind =		packet_bind_spkt,
	.connect =	sock_no_connect,
	.socketpair =	sock_no_socketpair,
	.accept =	sock_no_accept,
	.getname =	packet_getname_spkt,
	.poll =		datagram_poll,
	.ioctl =	packet_ioctl,
	.gettstamp =	sock_gettstamp,
	.listen =	sock_no_listen,
	.shutdown =	sock_no_shutdown,
	.sendmsg =	packet_sendmsg_spkt,
	.recvmsg =	packet_recvmsg,
	.mmap =		sock_no_mmap,
};

static const struct proto_ops packet_ops = { // SOCK_RAW
	.family =	PF_PACKET,
	.owner =	THIS_MODULE,
	.release =	packet_release,
	.bind =		packet_bind,
	.connect =	sock_no_connect,
	.socketpair =	sock_no_socketpair,
	.accept =	sock_no_accept,
	.getname =	packet_getname,
	.poll =		packet_poll,
	.ioctl =	packet_ioctl,
	.gettstamp =	sock_gettstamp,
	.listen =	sock_no_listen,
	.shutdown =	sock_no_shutdown,
	.setsockopt =	packet_setsockopt,
	.getsockopt =	packet_getsockopt,
	.sendmsg =	packet_sendmsg,
	.recvmsg =	packet_recvmsg,
	.mmap =		packet_mmap,
};
```

Далее после настройки сокета с помощью системных вызовов `setsockopt`, `bind` передача пакетов из ядра происходит через работу функции `packet_recvmsg`. Процесс передачи проходит следующие шаги:

1. Пользовательский процесс использует системный вызов `recvmsg` или `read`;
2. Контекст пользовательского процесса переходит в контекст ядра и вызывает функцию `packet_recvmsg`, которая хранится в структуре `sock`;
3. Получение данных, принятых сокетом, в функции `skb_recv_datagram`;
4. Копирование в пользовательский буфер виртуального заголовка канального уровня при его наличии;
5. Копирование данных пакета в пользовательский буфер в функции `skb_copy_datagram_msg`;
6. Копирование метаданных в структуру `sockaddr_ll` структуры `sk_buff`;
7. Уменьшение счётчика владения структурой `sk_buff`.

```c
// contrib/linux-6.18/net/socket.c
// Пример функции работы системного вызова recvmsg.
static inline int sock_recvmsg_nosec(struct socket *sock, struct msghdr *msg,
				     int flags)
{
	int ret = INDIRECT_CALL_INET(READ_ONCE(sock->ops)->recvmsg,
				     inet6_recvmsg,
				     inet_recvmsg, sock, msg,
				     msg_data_left(msg), flags);
	if (trace_sock_recv_length_enabled())
		call_trace_sock_recv_length(sock->sk, ret, flags);
	return ret;
}

/* ... */

// Пример фунции по обработке системного вызова recvmsg.
SYSCALL_DEFINE3(recvmsg, int, fd, struct user_msghdr __user *, msg,
		unsigned int, flags)
{
	// Вызывает sock_recvmsg_nosec.
	return __sys_recvmsg(fd, msg, flags, true);
}
```

```c
// contrib/linux-6.18/net/packet/af_packet.c
// Пример функции чтения пакета из сокета.
static int packet_recvmsg(struct socket *sock, struct msghdr *msg, size_t len,
			  int flags)
{
	struct sock *sk = sock->sk;
	struct sk_buff *skb;
	int copied, err;
	int vnet_hdr_len = READ_ONCE(pkt_sk(sk)->vnet_hdr_sz);
	unsigned int origlen = 0;

	/* ... */

	// Получение данных, принятых сокетом.
	skb = skb_recv_datagram(sk, flags, &err);

	/* ... */

	if (skb == NULL)
		goto out;

	// Отключение режима ускорения для пользовательской программы.
	packet_rcv_try_clear_pressure(pkt_sk(sk));

	if (vnet_hdr_len) {
		// Копирование в пользовательский буфер виртуального
		// заголовка канального уровня.
		err = packet_rcv_vnet(msg, skb, &len, vnet_hdr_len);
		if (err)
			goto out_free;
	}

	// Проверка длины буфера для дальнейшего копирования.
	copied = skb->len;
	if (copied > len) {
		copied = len;
		msg->msg_flags |= MSG_TRUNC;
	}

	// Копирование данных пакета в пользовательский буфер.
	err = skb_copy_datagram_msg(skb, 0, msg, copied);
	if (err)
		goto out_free;

	// Заполнение метаданных о пакете
	if (sock->type != SOCK_PACKET) {
		struct sockaddr_ll *sll = &PACKET_SKB_CB(skb)->sa.ll;

		origlen = PACKET_SKB_CB(skb)->sa.origlen;
		sll->sll_family = AF_PACKET;
		sll->sll_protocol = (sock->type == SOCK_DGRAM) ?
			vlan_get_protocol_dgram(skb) : skb->protocol;
	}

	/* ... */

out_free:
	// Уменьшение счётчика владения структурой sk_buff.
	skb_free_datagram(sk, skb);
out:
	return err;
}
```

Отправка пакета происходит в функции `packet_sendmsg`. Процесс передачи проходит следующие шаги:
1. Пользовательский процесс использует системные вызовы `sendmsg`, `sendto` или `write`.
2. Контекст пользовательского процесса переходит в контекст ядра и вызывает функцию `packet_sendmsg`, которая хранится в структуре `sock`;
3. Создание структуры `sk_buff`;
4. Резервирование места для заголовка канального уровня;
5. Копирование пользовательского буфера в структуру sk_buff;
6. Валидация скопированных данных;
7. Отправка пакета в драйвер сетевого интерфейса.

```c
// contrib/linux-6.18/net/socket.c
// Пример функции работы системного вызова sendmsg.
static inline int sock_sendmsg_nosec(struct socket *sock, struct msghdr *msg)
{
	int ret = INDIRECT_CALL_INET(READ_ONCE(sock->ops)->sendmsg, inet6_sendmsg,
				     inet_sendmsg, sock, msg,
				     msg_data_left(msg));
	BUG_ON(ret == -EIOCBQUEUED);

	if (trace_sock_send_length_enabled())
		call_trace_sock_send_length(sock->sk, ret, 0);
	return ret;
}

/* ... */

// Пример фунции по обработке системного вызова sendmsg.
SYSCALL_DEFINE3(sendmsg, int, fd, struct user_msghdr __user *, msg, unsigned int, flags)
{
	// Вызывает sock_sendmsg_nosec.
	return __sys_sendmsg(fd, msg, flags, true);
}
```

```c
// contrib/linux-6.18/net/socket.c
// Пример отправки пакета через пользовательский буфер.
static int packet_snd(struct socket *sock, struct msghdr *msg, size_t len)
{
	struct sock *sk = sock->sk;
	DECLARE_SOCKADDR(struct sockaddr_ll *, saddr, msg->msg_name);
	struct sk_buff *skb;
	struct net_device *dev;
	__be16 proto;
	unsigned char *addr = NULL;
	int err, reserve = 0;
	struct sockcm_cookie sockc;
	struct virtio_net_hdr vnet_hdr = { 0 };
	int offset = 0;
	struct packet_sock *po = pkt_sk(sk);
	int vnet_hdr_sz = READ_ONCE(po->vnet_hdr_sz);
	int hlen, tlen, linear;
	int extra_len = 0;

	/* ... */

	// Создание структуры sk_buff.
	err = -ENOBUFS;
	hlen = LL_RESERVED_SPACE(dev);
	tlen = dev->needed_tailroom;
	linear = __virtio16_to_cpu(vio_le(), vnet_hdr.hdr_len);
	linear = max(linear, min_t(int, len, dev->hard_header_len));
	skb = packet_alloc_skb(sk, hlen + tlen, hlen, len, linear,
			       msg->msg_flags & MSG_DONTWAIT, &err);
	if (skb == NULL)
		goto out_unlock;

	// Установка смещения до начала данных пакета.
	skb_reset_network_header(skb);

	// Резервирование места для заголовка канального уровня.
	err = -EINVAL;
	if (sock->type == SOCK_DGRAM) {
		offset = dev_hard_header(skb, dev, ntohs(proto), addr, NULL, len);
		if (unlikely(offset < 0))
			goto out_free;
	} else if (reserve) {
		skb_reserve(skb, -reserve);
		if (len < reserve + sizeof(struct ipv6hdr) &&
		    dev->min_header_len != dev->hard_header_len)
			skb_reset_network_header(skb);
	}

	// Копирование пользовательского буфера в структуру sk_buff,
	err = skb_copy_datagram_from_iter(skb, offset, &msg->msg_iter, len);
	if (err)
		goto out_free;

	// Валидация данных пакета.
	if ((sock->type == SOCK_RAW &&
	     !dev_validate_header(dev, skb->data, len)) || !skb->len) {
		err = -EINVAL;
		goto out_free;
	}

	// Установка времени отправки пакета.
	skb_setup_tx_timestamp(skb, &sockc);

	/* ... */

	// Установка метаданных.
	skb->protocol = proto;
	skb->dev = dev;
	skb->priority = sockc.priority;
	skb->mark = sockc.mark;
	skb_set_delivery_type_by_clockid(skb, sockc.transmit_time, sk->sk_clockid);

	/* ... */

	// Отправка пакета в драйвер сетевого интерфейса.
	err = packet_xmit(po, skb);

	if (unlikely(err != 0)) {
		if (err > 0)
			err = net_xmit_errno(err);
		if (err)
			goto out_unlock;
	}

	dev_put(dev);

	return len;

out_free:
	kfree_skb(skb);
out_unlock:
	dev_put(dev);
out:
	return err;
}

// Пример функции отправки пакета через sendmsg.
static int packet_sendmsg(struct socket *sock, struct msghdr *msg, size_t len)
{
	struct sock *sk = sock->sk;
	struct packet_sock *po = pkt_sk(sk);

	// Проверка существования кольца TX.
	if (data_race(po->tx_ring.pg_vec))
		// Отправка пакетов в кольце TX.
		return tpacket_snd(po, msg);

	// Отправка переданного пакета через пользовательский буфер.
	return packet_snd(sock, msg, len);
}
```

Для получения и отправки пакетов при использовании общей памяти используются функции `tpacket_rcv` и `tpacket_snd` соответственно. Функция `tpacket_rcv` выполняет следующие шаги:

1. Проверка возможности работы с пакетом;
2. Определение длины пакета;
3. Обработка приходящего пакета фильтром cBPF, если он установлен;
4. Выставление флагов статуса пакета для передачи в пользовательское пространство;
5. Получение указателя на участок памяти для записи пакета;
6. Копирование данных пакета в кольцо RX;
7. Получение времени захвата;
8. Заполнение заголовков пакетов;
9. Заполнение метаданных после заголовка пакета.

```c
// contrib/linux-6.18/net/socket.c
// Пример записи пакета в кольцо RX.
static int tpacket_rcv(struct sk_buff *skb, struct net_device *dev,
		       struct packet_type *pt, struct net_device *orig_dev)
{
	/* ... */
	enum skb_drop_reason drop_reason = SKB_CONSUMED;
	struct sock *sk = NULL;
	struct packet_sock *po;
	struct sockaddr_ll *sll;
	union tpacket_uhdr h;
	u8 *skb_head = skb->data;
	int skb_len = skb->len;
	unsigned int snaplen, res;
	unsigned long status = TP_STATUS_USER;
	unsigned short macoff, hdrlen;
	unsigned int netoff;
	struct sk_buff *copy_skb = NULL;
	struct timespec64 ts;
	__u32 ts_status;
	unsigned int slot_id = 0;
	int vnet_hdr_sz = 0;

	// Проверка возможности работы с пакетом.
	BUILD_BUG_ON(TPACKET_ALIGN(sizeof(*h.h2)) != 32);
	BUILD_BUG_ON(TPACKET_ALIGN(sizeof(*h.h3)) != 48);

	if (skb->pkt_type == PACKET_LOOPBACK)
		goto drop;

	sk = pt->af_packet_priv;
	po = pkt_sk(sk);

	if (!net_eq(dev_net(dev), sock_net(sk)))
		goto drop;

	// Сдвиг заголовка канального уровня.
	if (dev_has_header(dev)) {
		if (sk->sk_type != SOCK_DGRAM)
			skb_push(skb, skb->data - skb_mac_header(skb));
		else if (skb->pkt_type == PACKET_OUTGOING) {
			skb_pull(skb, skb_network_offset(skb));
		}
	}

	// Определение длины пакета.
	snaplen = skb_frags_readable(skb) ? skb->len : skb_headlen(skb);

	// Обработка приходящего пакета фильтром cBPF.
	res = run_filter(skb, sk, snaplen);
	if (!res)
		goto drop_n_restore;

	/* ... */

	// Выставление флагов статуса пакета для передачи
	// в пользовательское пространство.
	if (skb->ip_summed == CHECKSUM_PARTIAL)
		status |= TP_STATUS_CSUMNOTREADY;
	else if (skb->pkt_type != PACKET_OUTGOING &&
		 skb_csum_unnecessary(skb))
		status |= TP_STATUS_CSUM_VALID;
	if (skb_is_gso(skb) && skb_is_gso_tcp(skb))
		status |= TP_STATUS_GSO_TCP;

	if (snaplen > res)
		snaplen = res;

	// Расчёт смещения до канального и сетевого заголовка.
	if (sk->sk_type == SOCK_DGRAM) {
		macoff = netoff = TPACKET_ALIGN(po->tp_hdrlen) + 16 +
				  po->tp_reserve;
	} else {
		unsigned int maclen = skb_network_offset(skb);
		netoff = TPACKET_ALIGN(po->tp_hdrlen +
				       (maclen < 16 ? 16 : maclen)) +
				       po->tp_reserve;
		vnet_hdr_sz = READ_ONCE(po->vnet_hdr_sz);
		if (vnet_hdr_sz)
			netoff += vnet_hdr_sz;
		macoff = netoff - maclen;
	}

	/* ... */

	// Получение указателя на участок памяти для записи пакета.
	spin_lock(&sk->sk_receive_queue.lock);
	h.raw = packet_current_rx_frame(po, skb,
					TP_STATUS_KERNEL, (macoff+snaplen));
	if (!h.raw)
		goto drop_n_account;

	if (po->tp_version <= TPACKET_V2) {
		slot_id = po->rx_ring.head;
		if (test_bit(slot_id, po->rx_ring.rx_owner_map))
			goto drop_n_account;
		__set_bit(slot_id, po->rx_ring.rx_owner_map);
	}

	/* ... */

	spin_unlock(&sk->sk_receive_queue.lock);
	// Копирование данных пакета в кольцо RX.
	skb_copy_bits(skb, 0, h.raw + macoff, snaplen);

	// Получение времени захвата.
	ts_status = tpacket_get_timestamp(skb, &ts,
					  READ_ONCE(po->tp_tstamp) |
					  SOF_TIMESTAMPING_SOFTWARE);
	if (!ts_status)
		ktime_get_real_ts64(&ts);

	status |= ts_status;

	// Заполнение заголовков пакетов.
	switch (po->tp_version) {
	case TPACKET_V1:
		h.h1->tp_len = skb->len;
		h.h1->tp_snaplen = snaplen;
		h.h1->tp_mac = macoff;
		h.h1->tp_net = netoff;
		h.h1->tp_sec = ts.tv_sec;
		h.h1->tp_usec = ts.tv_nsec / NSEC_PER_USEC;
		hdrlen = sizeof(*h.h1);
		break;
	case TPACKET_V2:
		h.h2->tp_len = skb->len;
		h.h2->tp_snaplen = snaplen;
		h.h2->tp_mac = macoff;
		h.h2->tp_net = netoff;
		h.h2->tp_sec = ts.tv_sec;
		h.h2->tp_nsec = ts.tv_nsec;
		if (skb_vlan_tag_present(skb)) {
			h.h2->tp_vlan_tci = skb_vlan_tag_get(skb);
			h.h2->tp_vlan_tpid = ntohs(skb->vlan_proto);
			status |= TP_STATUS_VLAN_VALID | TP_STATUS_VLAN_TPID_VALID;
		} else if (unlikely(sk->sk_type == SOCK_DGRAM && eth_type_vlan(skb->protocol))) {
			h.h2->tp_vlan_tci = vlan_get_tci(skb, skb->dev);
			h.h2->tp_vlan_tpid = ntohs(skb->protocol);
			status |= TP_STATUS_VLAN_VALID | TP_STATUS_VLAN_TPID_VALID;
		} else {
			h.h2->tp_vlan_tci = 0;
			h.h2->tp_vlan_tpid = 0;
		}
		memset(h.h2->tp_padding, 0, sizeof(h.h2->tp_padding));
		hdrlen = sizeof(*h.h2);
		break;
	case TPACKET_V3:
		h.h3->tp_status |= status;
		h.h3->tp_len = skb->len;
		h.h3->tp_snaplen = snaplen;
		h.h3->tp_mac = macoff;
		h.h3->tp_net = netoff;
		h.h3->tp_sec  = ts.tv_sec;
		h.h3->tp_nsec = ts.tv_nsec;
		memset(h.h3->tp_padding, 0, sizeof(h.h3->tp_padding));
		hdrlen = sizeof(*h.h3);
		break;
	default:
		BUG();
	}

	// Заполнение метаданных после заголовка пакета.
	sll = h.raw + TPACKET_ALIGN(hdrlen);
	sll->sll_halen = dev_parse_header(skb, sll->sll_addr);
	sll->sll_family = AF_PACKET;
	sll->sll_hatype = dev->type;
	sll->sll_protocol = (sk->sk_type == SOCK_DGRAM) ?
		vlan_get_protocol_dgram(skb) : skb->protocol;
	sll->sll_pkttype = skb->pkt_type;
	if (unlikely(packet_sock_flag(po, PACKET_SOCK_ORIGDEV)))
		sll->sll_ifindex = orig_dev->ifindex;
	else
		sll->sll_ifindex = dev->ifindex;

	smp_mb();

 /* ... */

	// Установка статуса блоку пакетов о готовности для чтения.
	if (po->tp_version <= TPACKET_V2) {
		spin_lock(&sk->sk_receive_queue.lock);
		__packet_set_status(po, h.raw, status);
		__clear_bit(slot_id, po->rx_ring.rx_owner_map);
		spin_unlock(&sk->sk_receive_queue.lock);
		sk->sk_data_ready(sk);
	} else if (po->tp_version == TPACKET_V3) {
		prb_clear_blk_fill_status(&po->rx_ring);
	}

drop_n_restore:
	if (skb_head != skb->data && skb_shared(skb)) {
		skb->data = skb_head;
		skb->len = skb_len;
	}
drop:
	sk_skb_reason_drop(sk, skb, drop_reason);
	return 0;

 /* ... */
}
```

Далее рассмотрим функцию `tpacket_snd`, описав шаги отправки пакетов:

0. Ожидается корректная заполненности блока пакетами;
1. Получение индекса сетевого интерфейса для отправки;
2. Резервирование места для заголовка канального уровня и расчёт максимальной длины пакета;
3. Обработка блока:
     1. Получение пакета из блока и его длины;
     2. Создание структуры `sk_buff`;
     3. Заполнение структуры `sk_buff` страницами памяти кольца — это позволяет отправлять пакеты без лишнего копирования в буфер структуры `sk_buff`;
     4. Обработка ошибки и перемещение в кольце;
     5. Ожидание новых пакетов;
     6. Установление флага и отправка готового пакета;
     7. Переход к следующему пакету;
     8. Установка статуса об отправке;
4. Установка статуса об ошибке или о результе отправки.

```C
// contrib/linux-6.18/net/socket.c
// Пример чтения пакетов из кольца TX и их отправка.
static int tpacket_snd(struct packet_sock *po, struct msghdr *msg)
{
	struct sk_buff *skb = NULL;
	struct net_device *dev;
	struct virtio_net_hdr *vnet_hdr = NULL;
	struct sockcm_cookie sockc;
	__be16 proto;
	int err, reserve = 0;
	void *ph;
	DECLARE_SOCKADDR(struct sockaddr_ll *, saddr, msg->msg_name);
	bool need_wait = !(msg->msg_flags & MSG_DONTWAIT);
	int vnet_hdr_sz = READ_ONCE(po->vnet_hdr_sz);
	unsigned char *addr = NULL;
	int tp_len, size_max;
	void *data;
	int len_sum = 0;
	int status = TP_STATUS_AVAILABLE;
	int hlen, tlen, copylen = 0;
	long timeo;

	mutex_lock(&po->pg_vec_lock);

	/* ... */

	// Получение индекса сетевого интерфейса для отправки.
	if (likely(saddr == NULL)) {
		dev	= packet_cached_dev_get(po);
		proto	= READ_ONCE(po->num);
	} else {
		err = -EINVAL;
		if (msg->msg_namelen < sizeof(struct sockaddr_ll))
			goto out;
		if (msg->msg_namelen < (saddr->sll_halen
					+ offsetof(struct sockaddr_ll,
						sll_addr)))
			goto out;
		proto	= saddr->sll_protocol;
		dev = dev_get_by_index(sock_net(&po->sk), saddr->sll_ifindex);
		if (po->sk.sk_socket->type == SOCK_DGRAM) {
			if (dev && msg->msg_namelen < dev->addr_len +
				   offsetof(struct sockaddr_ll, sll_addr))
				goto out_put;
			addr = saddr->sll_addr;
		}
	}

	err = -ENXIO;
	if (unlikely(dev == NULL))
		goto out;
	err = -ENETDOWN;
	if (unlikely(!(dev->flags & IFF_UP)))
		goto out_put;

	// Отправка переданного сообщения сокету.
	sockcm_init(&sockc, &po->sk);
	if (msg->msg_controllen) {
		err = sock_cmsg_send(&po->sk, msg, &sockc);
		if (unlikely(err))
			goto out_put;
	}

	// Резервирование места для заголовка канального уровня
	// и расчёт максимальной длины пакета.
	if (po->sk.sk_socket->type == SOCK_RAW)
		reserve = dev->hard_header_len;
	size_max = po->tx_ring.frame_size
		- (po->tp_hdrlen - sizeof(struct sockaddr_ll));

	if ((size_max > dev->mtu + reserve + VLAN_HLEN) && !vnet_hdr_sz)
		size_max = dev->mtu + reserve + VLAN_HLEN;

	/* ... */

	do {
		// Получение пакета из блока
		ph = packet_current_frame(po, &po->tx_ring,
					  TP_STATUS_SEND_REQUEST);
		if (unlikely(ph == NULL)) {
			// Ожидание новых пакетов.
			if (need_wait && packet_read_pending(&po->tx_ring)) {
				timeo = wait_for_completion_interruptible_timeout(&po->skb_completion, timeo);
				if (timeo <= 0) {
					err = !timeo ? -ETIMEDOUT : -ERESTARTSYS;
					goto out_put;
				}
				/* check for additional frames */
				continue;
			} else
				break;
		}

		// Получение длины пакета.
		skb = NULL;
		tp_len = tpacket_parse_header(po, ph, size_max, &data);
		if (tp_len < 0)
			goto tpacket_error;

		// Добавление заголовка виртуального интерфейса.
		status = TP_STATUS_SEND_REQUEST;
		hlen = LL_RESERVED_SPACE(dev);
		tlen = dev->needed_tailroom;
		if (vnet_hdr_sz) {
			vnet_hdr = data;
			data += vnet_hdr_sz;
			tp_len -= vnet_hdr_sz;
			if (tp_len < 0 ||
			    __packet_snd_vnet_parse(vnet_hdr, tp_len)) {
				tp_len = -EINVAL;
				goto tpacket_error;
			}
			copylen = __virtio16_to_cpu(vio_le(),
						    vnet_hdr->hdr_len);
		}
		// Создание структуры sk_buff.
		copylen = max_t(int, copylen, dev->hard_header_len);
		skb = sock_alloc_send_skb(&po->sk,
				hlen + tlen + sizeof(struct sockaddr_ll) +
				(copylen - dev->hard_header_len),
				!need_wait, &err);

		if (unlikely(skb == NULL)) {
			if (likely(len_sum > 0))
				err = len_sum;
			goto out_status;
		}
		// Заполнение структуры sk_buff страницами памяти кольца,
		// что, позволяет отправлять пакеты без лишнего копирования
		// в буфер структуры sk_buff.
		tp_len = tpacket_fill_skb(po, skb, ph, dev, data, tp_len, proto,
					  addr, hlen, copylen, &sockc);
		if (likely(tp_len >= 0) &&
		    tp_len > dev->mtu + reserve &&
		    !vnet_hdr_sz &&
		    !packet_extra_vlan_len_allowed(dev, skb))
			tp_len = -EMSGSIZE;

		// Обработка ошибки и перемещение в кольце.
		if (unlikely(tp_len < 0)) {
tpacket_error:
			if (packet_sock_flag(po, PACKET_SOCK_TP_LOSS)) {
				__packet_set_status(po, ph,
						TP_STATUS_AVAILABLE);
				packet_increment_head(&po->tx_ring);
				kfree_skb(skb);
				continue;
			} else {
				status = TP_STATUS_WRONG_FORMAT;
				err = tp_len;
				goto out_status;
			}
		}

		/* ... */

		// Установка статуса об отправке.
		skb->destructor = tpacket_destruct_skb;
		__packet_set_status(po, ph, TP_STATUS_SENDING);
		packet_inc_pending(&po->tx_ring);

		status = TP_STATUS_SEND_REQUEST;
		// Отправка готового пакета.
		err = packet_xmit(po, skb);
		if (unlikely(err != 0)) {
			if (err > 0)
				err = net_xmit_errno(err);
			if (err && __packet_get_status(po, ph) ==
				   TP_STATUS_AVAILABLE) {
				skb = NULL;
				goto out_status;
			}
			err = 0;
		}
		// Переход к следующему пакету.
		packet_increment_head(&po->tx_ring);
		len_sum += tp_len;
	} while (1);

	err = len_sum;
	goto out_put;

out_status:
	// Установка статуса об ошибке или о результе отправки.
	__packet_set_status(po, ph, status);
	kfree_skb(skb);
out_put:
	dev_put(dev);
out:
	mutex_unlock(&po->pg_vec_lock);
	return err;
}
```

## Разбор примеров

Рассмотрим теперь работу с системой «AF_PACKET» из пользовательского пространства. Все примеры по использованию системы приведены в директории `src/af_packet` в данном репозитории.

### Настройка сокета

Открытие сокета происходит через системный вызов `socket`. Параметрами вызова являются:

1. Семейство сокетов `AF_PACKET`;
2. Тип сокета: `SOCK_RAW` или `SOCK_DGRAM`;
3. Тип протокола следующего уровня в сетевом порядке байт.

```c
// src/af_packet/classic.c
// Пример открытия сокета AF_PACKET, который будет получать
// пакеты с заголовком канального уровня Ethernet II и любым
// следующим протоколом (ETH_P_ALL).
sock_fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
if (sock_fd == -1) {
	perror("Socket error");
	return -1;
}
```

Далее необходимо определится со способом чтения/отправки пакетов. В разделе про [доступные режимы](#доступные-режимы) указано, что можно использовать общую память между ядром и программой для передачи сетевых пакетов или использовать системные вызовы для чтения и записи пакетов. Использование общей памяти является предпочтительным, так как многократно ускоряет процесс захвата из-за отсутствия переключения контекса процесса и контекса ядра.

```c
// src/af_packet/rings.c
// Пример функции создания колец 3-й версии.
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
```

После получения дескриптора сокета, на него будут поступать пакеты со всех доступных интерфейсов, которые удовлетворяют типу протокола, переданного третьим параметром. Чтобы настроить процесс захвата с определённого интерфейса необходимо знать его индекс в сетевом подсистеме ядра «Linux». Если индекс сетевого интерфейса, с которого ожидается захват пакетов, неизвестен, то получить его можно с помощью функции `if_nametoindex`, передав ей имя интерфейса.

```c
// src/af_packet/classic.c
// Пример получения номера сетевого интерфейса по его имени.
ifindex = if_nametoindex(ifname);
if (ifindex == 0) {
	perror("Get interface index");
	close(sock_fd);
	return -1;
}
```

Установка источника пакетов происходит с помощью системного вызова `bind` с передачей ядру в структуре `sockaddr_ll` семейства сокета `AF_PACKET`, типа протокола следующего уровня и индекса сетевого интерфейса.

```c
// src/af_packet/classic.c
// Пример функции установки источника пакетов для сокета.
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
```

Чтобы получать любые пакеты, получаемые сетевым интерфейсом, необходимо перевести его в режим «прослушки». Для этого необходимо использовать системный вызов `setsockopt` с параметрами `SOL_PACKET` и `PACKET_ADD_MEMBERSHIP`, передав значение `PACKET_MR_PROMISC` и индекс сетевого интерфейса.

```c
// src/af_packet/classic.c
// Пример функции переключения интерфейса в прослушивающий режим (promisc mode).
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
```

Чтобы ускорить захват пакетов в несколько раз, нужно поменять принцип работы очереди пакетов. Для этого требуется установить режим работы очереди/очередей пакетов через системный вызов `setsockopt` с параметрами `SOL_PACKET` и `PACKET_FANOUT`, передав нужный режим в аргументах (см. [доступные режимы](#доступные-режимы)).

```c
// src/af_packet/classic.c
// Пример функции установки режима распределения пакетов по очередям.
int
set_fanout(int sock_fd, int fanout_group_id) {
	int arg = (fanout_group_id | (FANOUT_MODE << 16));
	if (setsockopt(sock_fd, SOL_PACKET, PACKET_FANOUT, &arg, sizeof(arg)) == -1) {
		perror("Set fanout\n");
		return -1;
	}
	return 0;
}
```

Дополнительной настройкой является установка размерности времени захвата пакетов в микросекундах через параметр `SO_TIMESTAMP` или в наносекундах через параметр `SO_TIMESTAMPNS`. Без данной настройки ядро «Linux» не будет передавать время захвата пакета через системный вызов `recvmsg`.

```c
// src/af_packet/classic.c
// Пример функции включения сохранения времени захвата пакетов.
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
```

Также дополнительной настройкой является установка времени ожидания для системных вызовов `read` и `recvmsg` через системный вызов `setsockopt` с параметрами `SOL_SOCKET` и `SO_RCVTIMEO`. Иначе процесс будет бесконечно ожидать поступления данных на сокет.

```c
// src/af_packet/classic.c
// Пример функции установки времени ожидания для функции чтения из сокета.
int
set_recv_timeout(int sock_fd) {
	struct timeval tv = {1, 0};

	// Системный вызов настройки сокета.
	// Подробнее: https://man7.org/linux/man-pages/man2/setsockopt.2.html
	if (setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
		perror("Set timeout");
		return -1;
	}
	return 0;
}
```

### Захват и отправка сетевых пакетов

#### Системные вызовы

Простое чтение и простая отправка пакетов доступны через системные вызовы `read`/`write` или `recvmsg`/`sendto`. Всё, что необходимо подготовить для использования этих методов передачи сетевых пакетов — это буфер памяти, в который будут копироваться полученные сетевым интерфейсом пакеты или из которого они будут читаться.

```c
// src/af_packet/classic.c
// Пример получения пакетов с помощью вызова read.
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

	struct timeval tv;
	int ret = ioctl(sock_fd, SIOCGSTAMP, &tv);
	if (ret < 0) {
		perror("Get time by ioctl");
		return;
	}

	LOCK_PRINT();
	printf("Received packet with len %d\n", len);
	printf("Time %ld:%ld\n", tv.tv_sec, tv.tv_usec);
	print_first_34_bytes(buffer, len);
	UNLOCK_PRINT();
	++pkts_count;
}
```

При этом использование системного вызова `recvmsg` позволяет получать время захвата последнего полученного пакета без необходимости использовать дополнительный системный вызов `ioctl` для получения тех же данных.

```c
// src/af_packet/classic.c
// Пример получения пакетов с помощью вызова recvmsg.
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
```

Разницы же в использовании системных вызовов `write`/`sendto` нет, поэтому в примерах приведён код только для вызова `write`.

```c
// src/af_packet/classic.c
// Пример отправки пакетов с помощью вызова write.
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
```

#### Кольца RX/TX

Работа с кольцами приема и отправки пакетов требует большего количества кода. Так вместо того, чтобы ожидать новых пакетов необходимо использовать системные вызовы `epoll`/`poll`/`select` для получения уведомления от ядра о заполнении блока пакетами, или же в бесконечном цикле проверять доступность блока для чтения пакетов.

```c
// src/af_packet/rings.c
// Пример проверки готовности блока пакетов для их чтения
// и использования системного вызова poll для ожидания пакетов.
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
```

Далее необходимо пройтись по блоку и обработать каждый из полученных пакетов.

```c
// src/af_packet/rings.c
// Пример обработки пакетов из кольца RX.
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
```

Отправка пакетов обеспечивается простой записью пакетов в выделенное для этого место блока и системным вызовом `send`, но без передачи данных. Пусть использование общей памяти не избавляет от использования системных вызовов, но заметно снижает их количество и исключает лишние копирования данных.

```c
// src/af_packet/rings.c
// Пример отправки пакетов через кольцо TX.
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
```

## Заключение

Из разбора примеров работы системы «AF_PACKET» и разбора её архитектуры становится ясно, что при захвате пакетов происходит два копирования пакета (в пространство ядра и в пространство пользователя), а при их отправке происходит одно копирование: из пользовательского пространства в пространство ядра. Добавляя к этому необходимость использования системных вызовов для передачи дополнительных данных и управляющих команд, получаем границу использования системы в решениях, где обрабатывается сетевой трафик со скоростью, не превышающей 10 Гб/с [3].

Ранее была предложена реализации 4-й версии «PACKET_MMAP», позволяющая реализовать копирование пакетов напрямую в пользовательское пространство, но она не была принята в исходный код ядра [4].

Чтобы драйвер сетевого устройства поддерживал систему «AF_PACKET» ему необходимо лишь уметь взаимодействовать с сетевым стеком ядра «Linux». Это означает, что любое сетевое устройство дает программную возможность захватывать пакеты при скорости до 10 Гб/с.

## Полезные материалы

1. [Список значений поля Ethertype заголовка Ethernet II](https://www.iana.org/assignments/ieee-802-numbers/ieee-802-numbers.xhtml)
2. [Описание системного вызова setsockopt](https://man7.org/linux/man-pages/man2/setsockopt.2.html)
3. [Описание системного вызова bind](https://man7.org/linux/man-pages/man2/bind.2.html)
4. [Описание системного вызова socket](https://man7.org/linux/man-pages/man2/socket.2.html)
5. [Описание функции if_indextoname](https://man7.org/linux/man-pages/man3/if_indextoname.3.html)
6. [Описание доступа к данным command message](https://man7.org/linux/man-pages/man3/cmsg.3.html)
7. [Описание системного вызова recvmsg](https://man7.org/linux/man-pages/man3/recvmsg.3p.html)
8. [Описание системного вызова read](https://man7.org/linux/man-pages/man2/read.2.html)
9. [Описание системного вызова ioctl](https://man7.org/linux/man-pages/man2/ioctl.2.html)
10. [Описание системного вызова write](https://man7.org/linux/man-pages/man2/write.2.html)
11. [Описание настройки планировщика задач](https://man7.org/linux/man-pages/man3/pthread_attr_setaffinity_np.3.html)
12. [Описание системного вызова mmap](https://man7.org/linux/man-pages/man2/mmap.2.html)

## Источники

1. [Описание системы «AF_PACKET»](https://man7.org/linux/man-pages/man7/packet.7.html)
2. [Документация ядра «Linux» о механизме «PACKET_MMAP»](https://www.kernel.org/doc/html/latest/networking/packet_mmap.html)
3.  [Ларин Д.В., Гетьман А.И. Средства захвата и обработки высокоскоростного сетевого трафика. Труды ИСП РАН, том 33, вып. 4, 2021 г., стр. 49-68. DOI: 10.15514/ISPRAS–2021–33(4)–4](https://www.ispras.ru/proceedings/docs/2021/33/4/isp_33_2021_4_49.pdf)
4.  [Презентация «AF_PACKET v4 and PACKET_ZEROCOPY»](https://netdevconf.info/2.2/slides/karlsson-afpacket-talk.pdf)

