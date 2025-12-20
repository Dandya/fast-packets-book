# Анализ системы AF_XDP

***

В данном разделе разбирается устройство системы AF_XDP, а также примеры её использования.

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

Аналогично системе «AF_PACKET» использует интерфейс сокетов ядра «Linux» для настройки режима работы. А для передачи пакетов между пользовательским процессом и ядром используется общая память пользовательского пространства, представляющая собой кольцевой буфер пакетов и имеющая название `UMEM`. При этом в ядре пакеты могут передаваться через структуры `sk_buff` или `xdp_buff`.

### Доступные режимы

Система «AF_XDP» поддерживает два режима работы:

1. `XDP_SKB` — режим работы, при котором данные для копирования в очередь берутся из структуры `sk_buff`;
2. `XDP_DRV` — режим работы, при котором сетевое устройство с помощью технологии DMA записывает пакеты напрямую в память пользовательского пространства.

С помощью установки флага `XDP_SHARED_UMEM` в аргументах системного вызова `bind` появляется возможность создания произвольного количества сокетов с одной разделяемой памятью, где метод распределения пакетов будет определяться XDP-программой загруженной в ядро «linux» и закрепленное за получением пакетов на определённом сетевом интерфейсе.

А при установке параметра `XDP_USE_SG` появляется возможность захватывать пакеты длиной до 9 килобайт, путём объединения буферов (фрагментов на уровне драйвера) в один дескриптор пакета. Захват больших пакетов также возможен через установку флага `XDP_UMEM_UNALIGNED_CHUNK_FLAG` с использованием технологии «hugepages», что с одной стороны увеличивает эффективность использования оперативной памяти из-за записи всех пакетов без пробелов и выравниваний, но увеличивает накладные расходы на управление памятью.

### Процесс передачи сетевых пакетов

Передача сетевых пакетов с использованием системы «AF_XDP» зависит не только от реализации сетевого стека ядра «Linux», но и от реализации драйвера. Проверить поддержку системы в драйвере можно по наличию флага `NETDEV_XDP_ACT_XSK_ZEROCOPY` в функции инициализации сетевого интерфейса.

```c
// contrib/linux-6.18/drivers/net/ethernet/intel/igb/igb_main.c
// Процесс инициализации доступных функций XDP.
static int igb_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	struct net_device *netdev;
	/* ... */
	netdev->xdp_features = NETDEV_XDP_ACT_BASIC | NETDEV_XDP_ACT_REDIRECT |
						NETDEV_XDP_ACT_XSK_ZEROCOPY;
	/* ... */
}
```

В работе системы ядро выполняет следующие функции:

- Настройка отображения памяти между пространством ядра и пространством пользователя;
- Настройка DMA для передачи пакетов;
- Загрузка в ядро XDP-программы;
- Управление памятью `UMEM`, а также кольцами `RX`, `TX`, `FR`, `CR`.

Тогда как драйвер реализует следующие функции:

- Работа с памятью;
- Обработка прерываний;
- Фильтрация пакетов с помощью XDP.


Таким образом, большинство драйверов могут реализовать «zero-copy» режим передачи пакетов без необходимости вностить множество изменений в исходный код драйвера и реализовывать доступ из пространства пользователя.

Для изучения процесса передачи сетевых пакетов следует обратиться к исходному коду системы в файле `contrib/linux-6.18/net/xdp/xsk.c`, `contrib/linux-6.18/net/xdp/xsk_buff_pool.c` и к части функций драйвера «IGB»: `contrib/linux-6.18/drivers/net/ethernet/intel/igb/igb_main.c` и `contrib/linux-6.18/drivers/net/ethernet/intel/igb/igb_xsk.c`.

Получение и отправка пакетов происходит с помощью работы с участком памяти `UMEM`, которая становится доступна драйверу или сетевому стеку при загрузке в них XDP-программы распределения пакетов.

```c
// contrib/linux-6.18/drivers/net/ethernet/intel/igb/igb_main.c
// Пример функций установки XDP-программы в режиме XDP_DRV.
// Данная функция передаётся в структуре net_device_ops.
static int igb_xdp(struct net_device *dev, struct netdev_bpf *xdp)
{
	struct igb_adapter *adapter = netdev_priv(dev);

	switch (xdp->command) {
	case XDP_SETUP_PROG:
		// Установка только XDP-программы.
		return igb_xdp_setup(dev, xdp);
	case XDP_SETUP_XSK_POOL:
		// Настройка сокета AF_XDP
		return igb_xsk_pool_setup(adapter, xdp->xsk.pool,
					  xdp->xsk.queue_id);
	default:
		return -EINVAL;
	}
}
```

```c
// contrib/linux-6.18/drivers/net/ethernet/intel/igb/igb_xsk.c
// Пример функций установки и удаления памяти UMEM и
// очереди пакетов с номером qid в режиме XDP_DRV.
static int igb_xsk_pool_enable(struct igb_adapter *adapter,
			       struct xsk_buff_pool *pool,
			       u16 qid)
{
	struct net_device *netdev = adapter->netdev;
	struct igb_ring *rx_ring;
	bool if_running;
	int err;

	// Проверка номера очереди.
	if (qid >= adapter->num_rx_queues)
		return -EINVAL;

	if (qid >= netdev->real_num_rx_queues ||
	    qid >= netdev->real_num_tx_queues)
		return -EINVAL;

	// Настройка DMA ядром.
	err = xsk_pool_dma_map(pool, &adapter->pdev->dev, IGB_RX_DMA_ATTR);
	if (err)
		return err;

	/* ... */

	// Создание новых очередей пакетов с использованием UMEM памяти.
	if (if_running) {
		err = igb_realloc_rx_buffer_info(rx_ring, true);

		/* ... */

	}

	return 0;
}

static int igb_xsk_pool_disable(struct igb_adapter *adapter, u16 qid)
{
	struct xsk_buff_pool *pool;
	struct igb_ring *rx_ring;
	bool if_running;
	int err;

	// Получение памяти UMEM.
	pool = xsk_get_pool_from_qid(adapter->netdev, qid);
	if (!pool)
		return -EINVAL;

	/* ... */

	// Удаление очередей, использующих UMEM память.
	if (if_running)
		igb_txrx_ring_disable(adapter, qid);

	// Перенастройка DMA.
	xsk_pool_dma_unmap(pool, IGB_RX_DMA_ATTR);

	// Создание очередей на новой памяти.
	if (if_running) {
		err = igb_realloc_rx_buffer_info(rx_ring, false);
		if (err)
			return err;

		/* ... */

	}

	return 0;
}

int igb_xsk_pool_setup(struct igb_adapter *adapter,
		       struct xsk_buff_pool *pool,
		       u16 qid)
{
	return pool ?
			// Установка памяти UMEM и колец.
			igb_xsk_pool_enable(adapter, pool, qid) :
			// Их удаление.
			igb_xsk_pool_disable(adapter, qid);
}
```

```c
// fast-packets-book/contrib/linux-6.18/net/xdp/xsk_buff_pool.c
// Пример функции ядра, в которой происходит установка памяти UMEM в
// зависимости от режима работы.
int xp_assign_dev(struct xsk_buff_pool *pool,
		  struct net_device *netdev, u16 queue_id, u16 flags)
{
	bool force_zc, force_copy;
	struct netdev_bpf bpf;
	int err = 0;

	ASSERT_RTNL();

	force_zc = flags & XDP_ZEROCOPY;
	force_copy = flags & XDP_COPY;

	if (force_zc && force_copy)
		return -EINVAL;

	/* ... */

	// В случае режима XDP_SKB память UMEM не загружается в драйвер.
	if (force_copy)
		return 0;

 /* ... */

	bpf.command = XDP_SETUP_XSK_POOL;
	bpf.xsk.pool = pool;
	bpf.xsk.queue_id = queue_id;

	netdev_ops_assert_locked(netdev);
	// Иначе память передаётся при загрузке XDP-программы.
	err = netdev->netdev_ops->ndo_bpf(netdev, &bpf);
	if (err)
		goto err_unreg_pool;

	/* ... */

	return err;
}
```

После получения прерывания о поступлении пакета в рассмотренной функции `igb_poll` (см. [основы работы сетевого стека ядра «Linux»](Linux-network-stack.md)) в случае использования сокета «AF_XDP» в режиме `XDP_DRV` пакеты копируются в память `UMEM` сетевым устройством с помощью DMA, а после вызывается функция `igb_clean_rx_irq_zc` вместо `igb_clean_rx_irq`. Данная функция обеспечивает запись данных в сокет и фильтрацию пакета, который можно отправить в сокет, отправить вверх по сетевому стеку или отбросить. Таким образом, система «AF_XDP» позволяет не только захватывать сетевой трафик, но и не прерывать работу сетевого интерфейса. В режиме `XDP_SKB` пакет поступает в сетевой стек из функции `igb_clean_rx_irq`, где позже обработчиком копируется в память `UMEM` в функции `do_xdp_generic`.


```c
// contrib/linux-6.18/drivers/net/ethernet/intel/igb/igb_xsk.c
// Пример получения и фильтрации пакетов в режиме XDP_DRV.
static int igb_run_xdp_zc(struct igb_adapter *adapter, struct igb_ring *rx_ring,
			  struct xdp_buff *xdp, struct xsk_buff_pool *xsk_pool,
			  struct bpf_prog *xdp_prog)
{
	int err, result = IGB_XDP_PASS;
	u32 act;

	/* ... */

	// Выполнение XDP-программы.
	act = bpf_prog_run_xdp(xdp_prog, xdp);

	if (likely(act == XDP_REDIRECT)) {
		// Обновление кольца RX.
		err = xdp_do_redirect(adapter->netdev, xdp, xdp_prog);
		if (!err)
			return IGB_XDP_REDIR;

		/* ... */

	}

	switch (act) {
	case XDP_PASS:
		// Отправка пакета вверх по сетевому стеку.
		break;
	case XDP_TX:
		// Отправка пакета в обратную сторону.
		result = igb_xdp_xmit_back(adapter, xdp);
		if (result == IGB_XDP_CONSUMED)
			goto out_failure;
		break;
	default:
		bpf_warn_invalid_xdp_action(adapter->netdev, xdp_prog, act);
		fallthrough;

		/* ... */

	}

	return result;
}

int igb_clean_rx_irq_zc(struct igb_q_vector *q_vector,
			struct xsk_buff_pool *xsk_pool, const int budget)
{
	struct igb_adapter *adapter = q_vector->adapter;
	unsigned int total_bytes = 0, total_packets = 0;
	struct igb_ring *rx_ring = q_vector->rx.ring;
	u32 ntc = rx_ring->next_to_clean;
	struct bpf_prog *xdp_prog;
	unsigned int xdp_xmit = 0;
	bool failure = false;
	u16 entries_to_alloc;
	struct sk_buff *skb;

	// Получение XDP-программы.
	xdp_prog = READ_ONCE(rx_ring->xdp_prog);

	// Обработка полученных пакетов.
	while (likely(total_packets < budget)) {
		union e1000_adv_rx_desc *rx_desc;
		ktime_t timestamp = 0;
		struct xdp_buff *xdp;
		unsigned int size;
		int xdp_res = 0;

		// Получение дескриптора пакета.
		rx_desc = IGB_RX_DESC(rx_ring, ntc);
		size = le16_to_cpu(rx_desc->wb.upper.length);
		if (!size)
			break;

		/* ... */

		// Фильтрация пакета и определение номера сокета.
		xdp_res = igb_run_xdp_zc(adapter, rx_ring, xdp, xsk_pool,
					 xdp_prog);

		if (xdp_res) {
			if (likely(xdp_res & (IGB_XDP_TX | IGB_XDP_REDIR))) {
				// Пакет был передан в сокет.
				xdp_xmit |= xdp_res;
			} else if (xdp_res == IGB_XDP_EXIT) {
				failure = true;
				break;
			} else if (xdp_res == IGB_XDP_CONSUMED) {
				xsk_buff_free(xdp);
			}

			total_packets++;
			total_bytes += size;
			ntc++;
			if (ntc == rx_ring->count)
				ntc = 0;
			// Переход к следующему пакету.
			continue;
		}

		// Иначе пакет отправляется вверх по сетевому стеку.
		skb = igb_construct_skb_zc(rx_ring, xdp, timestamp);

		/* ... */

		napi_gro_receive(&q_vector->napi, skb);

		total_packets++;
	}

	rx_ring->next_to_clean = ntc;

	// Обновление регисторов сетевого устройства.
	if (xdp_xmit)
		igb_finalize_xdp(adapter, xdp_xmit);

	// Обновление статистики.
	igb_update_rx_stats(q_vector, total_packets, total_bytes);

	/* ... */

	if (xsk_uses_need_wakeup(xsk_pool)) {
		if (failure || rx_ring->next_to_clean == rx_ring->next_to_use)
			xsk_set_rx_need_wakeup(xsk_pool);
		else
			xsk_clear_rx_need_wakeup(xsk_pool);

		return (int)total_packets;
	}
	return failure ? budget : (int)total_packets;
}
```

```c
// contrib/linux-6.18/net/core/dev.c
// Пример получения и фильтрации пакетов в режиме XDP_SKB.
int do_xdp_generic(const struct bpf_prog *xdp_prog, struct sk_buff **pskb)
{
	struct bpf_net_context __bpf_net_ctx, *bpf_net_ctx;

	if (xdp_prog) {
		struct xdp_buff xdp;
		u32 act;
		int err;

		// Фильтрация пакета.
		bpf_net_ctx = bpf_net_ctx_set(&__bpf_net_ctx);
		act = netif_receive_generic_xdp(pskb, &xdp, xdp_prog);
		if (act != XDP_PASS) {
			switch (act) {
			case XDP_REDIRECT:
				// Отправка пакета в сокет.
				err = xdp_do_generic_redirect((*pskb)->dev, *pskb,
							      &xdp, xdp_prog);
				if (err)
					goto out_redir;
				break;
			case XDP_TX:
				// Отправка пакета в обратную сторону.
				generic_xdp_tx(*pskb, xdp_prog);
				break;
			}
			bpf_net_ctx_clear(bpf_net_ctx);
			return XDP_DROP;
		}
		bpf_net_ctx_clear(bpf_net_ctx);
	}
	return XDP_PASS;

	/* ... */
}
```

```c
// contrib/linux-6.18/net/xdp/xsk.c
// Пример функций работы с кольцом RX.

// Вызывается xdp_do_redirect и настраивает кольца.
static int __xsk_rcv_zc(struct xdp_sock *xs, struct xdp_buff_xsk *xskb, u32 len,
			u32 flags)
{
	u64 addr;
	int err;

	// Получение адреса фрагмента.
	addr = xp_get_handle(xskb, xskb->pool);
	// Запись в кольцо RX, что пакет доступен.
	err = xskq_prod_reserve_desc(xs->rx, addr, len, flags);
	if (err) {
		xs->rx_queue_full++;
		return err;
	}

	// Освобождение буфера.
	xp_release(xskb);
	return 0;
}


// Вызывается функцией xdp_do_generic_redirect и копирует пакет
// в режиме XDP_SKB.
static int __xsk_rcv(struct xdp_sock *xs, struct xdp_buff *xdp, u32 len)
{
	u32 frame_size = xsk_pool_get_rx_frame_size(xs->pool);
	void *copy_from = xsk_copy_xdp_start(xdp), *copy_to;
	u32 from_len, meta_len, rem, num_desc;
	struct xdp_buff_xsk *xskb;
	struct xdp_buff *xsk_xdp;
	skb_frag_t *frag;

	from_len = xdp->data_end - copy_from;
	meta_len = xdp->data - copy_from;
	rem = len + meta_len;

	// Если пакет умещается в один фрагмент.
	if (len <= frame_size && !xdp_buff_has_frags(xdp)) {
		int err;

		/* ... */
		// Копирование.
		memcpy(xsk_xdp->data - meta_len, copy_from, rem);
		xskb = container_of(xsk_xdp, struct xdp_buff_xsk, xdp);
		// Настройка колец.
		err = __xsk_rcv_zc(xs, xskb, len, 0);
		if (err) {
			xsk_buff_free(xsk_xdp);
			return err;
		}

		return 0;
	}

	num_desc = (len - 1) / frame_size + 1;

	/* ... */

	// Если пакет не умещается в один фрагмент.
	do {
		u32 to_len = frame_size + meta_len;
		u32 copied;

		xsk_xdp = xsk_buff_alloc(xs->pool);
		copy_to = xsk_xdp->data - meta_len;

		// Копирование.
		copied = xsk_copy_xdp(copy_to, &copy_from, to_len, &from_len, &frag, rem);
		rem -= copied;

		// Настройка колец.
		xskb = container_of(xsk_xdp, struct xdp_buff_xsk, xdp);
		__xsk_rcv_zc(xs, xskb, copied - meta_len, rem ? XDP_PKT_CONTD : 0);
		meta_len = 0;
	} while (rem);

	return 0;
}
```

Отправка пакетов в режиме `XDP_DRV` происходит при записи пакетов в память `UMEM` и чтении их сетевой картой с помощью DMA. Однако настройка регистров для отправки следующих пакетов происходит в `igb_clean_tx_irq` (см. [основы работы сетевого стека ядра «Linux»](Linux-network-stack.md)). В режиме `XDP_SKB` страницы памяти `UMEM` помещаются в структуру `sk_buff` и после отправляются на чтение сетевой картой, что исключает лишнее копирование данных. При этом ядро узнает о факте отправки пакетов с помощью системного вызова `sendto`.

```c
// contrib/linux-6.18/net/xdp/xsk.c
// Пример функций отправки пакетов.

// Функция создания sk_buff и заполения их страницами памяти.
// Вызывается функцией xsk_generic_xmit.
static struct sk_buff *xsk_build_skb_zerocopy(struct xdp_sock *xs,
					      struct xdp_desc *desc)
{
	struct xsk_buff_pool *pool = xs->pool;
	u32 hr, len, ts, offset, copy, copied;
	struct sk_buff *skb = xs->skb;
	struct page *page;
	void *buffer;
	int err, i;
	u64 addr;

	addr = desc->addr;
	buffer = xsk_buff_raw_get_data(pool, addr);

	if (!skb) {
		hr = max(NET_SKB_PAD, L1_CACHE_ALIGN(xs->dev->needed_headroom));

		// Создание структуры sk_buff.
		skb = sock_alloc_send_skb(&xs->sk, hr, 1, &err);
		if (unlikely(!skb))
			return ERR_PTR(err);

		skb_reserve(skb, hr);

		/* ... */
	} else {

		/* ... */

	}

	len = desc->len;
	ts = pool->unaligned ? len : pool->chunk_size;

	offset = offset_in_page(buffer);
	addr = buffer - pool->addrs;

	// Заполение структуры страницами памяти UMEM.
	for (copied = 0, i = skb_shinfo(skb)->nr_frags; copied < len; i++) {
		if (unlikely(i >= MAX_SKB_FRAGS))
			return ERR_PTR(-EOVERFLOW);

		// Получение страницы.
		page = pool->umem->pgs[addr >> PAGE_SHIFT];
		get_page(page);

		// Копирование дескриптора страницы в структуру sk_buff.
		copy = min_t(u32, PAGE_SIZE - offset, len - copied);
		skb_fill_page_desc(skb, i, page, offset, copy);

		copied += copy;
		addr += copy;
		offset = 0;
	}

	skb->len += len;
	skb->data_len += len;
	skb->truesize += ts;

	// Увеличивается счётчик пользователей структуры sk_buff.
	refcount_add(ts, &xs->sk.sk_wmem_alloc);

	return skb;
}

// Функция подтверждения отправки пакетов.
static int __xsk_sendmsg(struct socket *sock, struct msghdr *m, size_t total_len)
{
	bool need_wait = !(m->msg_flags & MSG_DONTWAIT);
	struct sock *sk = sock->sk;
	struct xdp_sock *xs = xdp_sk(sk);
	struct xsk_buff_pool *pool;
	int err;

	err = xsk_check_common(xs);
	if (err)
		return err;
	if (unlikely(need_wait))
		return -EOPNOTSUPP;
	if (unlikely(!xs->tx))
		return -ENOBUFS;

	if (sk_can_busy_loop(sk))
		sk_busy_loop(sk, 1);

	if (xs->zc && xsk_no_wakeup(sk))
		return 0;

	pool = xs->pool;
	if (pool->cached_need_wakeup & XDP_WAKEUP_TX) {
		if (xs->zc)
			// В режиме XDP_DRV генерируется программное прерывание
			// для igb_poll в функции igb_xsk_wakeup.
			return xsk_wakeup(xs, XDP_WAKEUP_TX);
		// Создание структур sk_buff и их отправка.
		return xsk_generic_xmit(sk);
	}
	return 0;
}
```

## Разбор примеров

Чтобы работать с программами на основе технологии «eBPF» необходимо установить дополнительные пакеты:

```sh
apt install clang libbpf-dev pkg-config
```

Работа с системой «AF_XDP» будет происходить с помощью библиотеки «xdptool», так как она является предпочтительным способом взаимодействия с «AF_XDP» и имеет хорошую документацию. Компиляция XDP-программ в код «eBPF» производится программой «clang», а для загрузки скомпилированных XDP-программ в ядро будут использоваться функции библиотеки «xdptool».

Все примеры по использованию системы приведены в директории `src/af_xdp` в данном репозитории.


### Настройка сокета

Чтобы захватывать пакеты с помощью системы «AF_XDP», необходимо зафиксировать каждую очередь приёма пакетов сетевого интерфейса за отдельным логическим процессором. Для этого достаточно выполнить скрипт `src/scripts/set_irq_affinity.sh`, передав название интерфейса, с которого будет происходить захват пакетов.

Далее необходимо загрузить в ядро скомпилированную программу. Для этого используется функция `xdp_program__attach`, а для выгрузки программы функция `xdp_program__detach`. Пример XDP-программы находится в файле `src/af_xdp/kern.c`.

```c
// src/af_xdp/user.c
// Пример функций загрузки и удаления XDP-программы из ядра.
static void
remove_xdp_program(void) {
	// Удаление XDP программы из сетевого интерфейса.
	// Подробнее: https://docs.ebpf.io/ebpf-library/libbpf/userspace/bpf_xdp_detach
	int err = xdp_program__detach(xdp_prog, opt_ifindex, opt_attach_mode, 0);
	/* ... */
}

static void
load_xdp_program(void) {
	/* ... */

	// Чтение объектного файла с байткодом eBPT.
	// Подробнее: https://docs.ebpf.io/ebpf-library/libxdp/functions/xdp_program__open_file
	xdp_prog = xdp_program__open_file(opt_xdp_path, NULL, NULL);
	err = libxdp_get_error(xdp_prog);
	if (err) {
		/* ... */
	}

	// Установка XDP программы в ядро и закрепление её за очередью сетевого интерфейса.
	err = xdp_program__attach(xdp_prog, opt_ifindex, opt_attach_mode, 0);
	if (err) {
		/* ... */
	}
}
```

Следующим шагом является создание памяти `UMEM` с помощью функции `xsk_umem__create`, которая выделяет память по границе страниц памяти заданного размера.

```c
// src/af_xdp/user.c
// Пример создания памяти UMEM.
static struct umem_info*
create_umem(void* buffer, uint64_t size) {
	struct umem_info* umem;
	// Заполнение настроек памяти UMEM.
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

	// Создание памяти.
	ret = xsk_umem__create(&umem->umem, buffer, size, &umem->pr, &umem->cr, &cfg);
	if (ret)
		exit_with_error(-ret);

	umem->buffer = buffer;
	return umem;
}

```

Чтобы открыть сокет, необходимо вызвать функцию `xsk_socket__create` c указанием:

1. индекса сетевого интерфейса;
2. номера очереди приёма/отправки пакетов сетевого интерфейса;
3. адреса памяти `UMEM`;
4. адреса колец для получения/отправки пакетов;

```c
// src/af_xdp/user.c
// Пример функции создания сокета.
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

	// Заполнение настроек сокета.
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
```

После создания сокета необходимо загрузить его в память XDP-программы в ядре «Linux», чтобы при фильтрации программа могла возвращать индекс сокета для передачи пакета. Также для получения пакетов дополнительно нужно освободить все пакеты, записав данные в кольцо `fill` памяти `UMEM`.

```c
// src/af_xdp/user.c
// Пример функции добавления номера сокета в память XDP-программы.
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
```

```c
// src/af_xdp/user.c
// Пример функции заполнения очереди fill свободными пакетами.
static void
configure_fill_ring(struct umem_info* umem) {
	int ret = 0;
	uint32_t idx = 0;

	// Резервирование слотов в очереди fill.
	// Подробнее: https://docs.ebpf.io/ebpf-library/libxdp/functions/xsk_ring_prod__reserve
	ret = xsk_ring_prod__reserve(&umem->pr, XSK_RING_PROD__DEFAULT_NUM_DESCS * 2, &idx); // fill_size
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
```

### Захват и отправка сетевых пакетов

Для захвата пакетов сначала проверяется доступность дескрипторов в кольце `RX` . Если пакеты отсутствуют, программа уведомляет ядро об ожидании пакетов через системный вызов `recvfrom`. Далее резервируются дескрипторы в очереди свободных буферов для последующего записи в них прочитанных пакетов/фрагментов. Для каждого принятого пакета извлекается его адрес в памяти `UMEM` и появляется возможность обработки пакета.

```c
// src/af_xdp/user.c
// Пример функции получения пакетов.
static void
rx_only(struct socket_info* xsk) {
	unsigned int rcvd, i, eop_cnt = 0;
	uint32_t idx_rx = 0, idx_fq = 0;
	int ret;

	// Просмотр количества доступных пакетов/фрагментов для чтения.
	// Подробнее: https://docs.ebpf.io/ebpf-library/libxdp/functions/xsk_ring_cons__peek/
	rcvd = xsk_ring_cons__peek(&xsk->rx, opt_batch_size, &idx_rx);
	if (!rcvd) {
		if (opt_busy_poll || xsk_ring_prod__needs_wakeup(&xsk->umem->pr)) {
			// Уведомление ядра об ожидании пакетов.
			// Подробнее: https://man7.org/linux/man-pages/man3/recvfrom.3p.html
			recvfrom(xsk_socket__fd(xsk->xsk), NULL, 0, MSG_DONTWAIT, NULL, NULL);
		}
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
```

Для отправки пакетов сначала резервируются дескрипторы в очереди `TX`. Если доступного места недостаточно, вызывается функция `complete_tx_only`, которая ждёт освобождения отправленных пакетов/фрагментов из очереди завершения `CR`, уведомляя ядро через системный вызов sendto о завершении передачи пакетов. Далее для каждого  освободившегося пакета/фрагмента заполняется дескриптор, содержащий адрес в памяти `UMEM` и длину данных. При этом данные были заранее скопированы в память `UMEM`. После цикл повторяется. Завершающий вызов complete_tx_only обеспечивает освобождение буферов, успешно переданных сетевому интерфейсу.

```c
// src/af_xdp/user.c
// Пример функций отправки пакетов.
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
```

## Заключение

Из разбора примеров работы системы «AF_XDP» и разбора её архитектуры становится ясно, что при захвате пакетов происходит либо одно копирование сетевого устройства в оперативную память с помощью DMA в режиме `XDP_DRV`, либо два копирования пакета в режиме `XDP_SKB`, а при отправке пакетов не происходит копирования пакета. Использование системных вызовов для передачи управляющих команд даёт границу использования системы в решениях, где обрабатывается сетевой трафик со скоростью, не превышающей 100 Гб/с [3].

Чтобы драйвер сетевого устройства поддерживал систему «AF_XDP» ему необходимо лишь уметь взаимодействовать с сетевым стеком ядра «Linux». Но для поддержки высоких скоростей захвата сетевого трафика ему необходимо реализовать следующее [4]:

- хранение XDP-пакета в непрерывной памяти;
- настройка доступа чтения и записи XDP-программы к памяти c XDP-пакетами;
- отключение больших пакетов при установке XDP-программы;
- повторное использование памяти для очередей.

Не все драйвера в исходном коде ядра «Linux» имеют поддержку системы «AF_XDP», что говорит о том, что система применима только для популярных сетевых устройств, что значительно снижает возможности использования данной системы.

## Полезные материалы

1. [Примеры использования технологии «eBPF»](https://github.com/xdp-project/bpf-examples)
2. [Документация библиотеки libbpf](https://docs.ebpf.io/ebpf-library/)

## Источники

1. [Magnus Karlsson and Bjorn T «The Path to DPDK Speeds for AF XDP»](http://oldvger.kernel.org/lpc_net2018_talks/lpc18_paper_af_xdp_perf-v2.pdf)
2. [Документация ядра «Linux» о «AF_XDP»](https://docs.kernel.org/networking/af_xdp.html)
3. [Ларин Д.В., Гетьман А.И. Средства захвата и обработки высокоскоростного сетевого трафика. Труды ИСП РАН, том 33, вып. 4, 2021 г., стр. 49-68. DOI: 10.15514/ISPRAS–2021–33(4)–4](https://www.ispras.ru/proceedings/docs/2021/33/4/isp_33_2021_4_49.pdf)
4. [Добавление поддержки AF_XDP в сетевой драйвер](https://people.redhat.com/lbiancon/conference/NetDevConf2020-0x14/add-xdp-on-driver.html)