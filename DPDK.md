# Анализ системы DPDK

***

В данном разделе разбирается устройство системы DPDK, а также примеры её использования.

***

1. [Описание архитектуры](#описание-архитектуры)
	1. [Применяемые механизмы и технологии](#применяемые-механизмы-и-технологии)
	2. [Доступные режимы](#доступные-режимы)
	3. [Процесс передачи сетевых пакетов](#процесс-передачи-сетевых-пакетов)
2. [Разбор примеров](#разбор-примеров)
	1. [Настройка сетевого интерфейса](#настройка-сетевого-интерфейса)
	2. [Захват и отправка сетевых пакетов](#захват-и-отправка-сетевых-пакетов)
3. [Заключение](#заключение)
4. [Полезные материалы](#полезные-материалы)
5. [Источники](#источники)

***

## Описание архитектуры

### Применяемые механизмы и технологии

Система «DPDK» уходит от использования сетевого стека ядра «Linux» и использует свои драйвера, работающие в пространстве пользователя. Но для передачи данных и прерываний между драйвером пользовательского пространства используются механизмы «VFIO» или «UIO» ядра «Linux» [1].

Оба механизма используются для предоставления доступа к памяти и регистрам PCI-устройства в пользовательском пространстве. При этом «VFIO» является более защищенной и надёжной, так как использует механизм «IOMMU», который обеспечивает доступ к памяти и регистрам PCI-устройства по виртуальным адресам, тогда как в «UIO» для обращении к памяти использует указатели на физическую память [2].

Помимо этого система «DPDK» фиксирует потоки захвата/отправки сетевых пакетов на ядрах процессора и управляет памятью в зависимости от настроек технологии «NUMA».

Для снижения накладных расходов при работе с памятью используется механизм «Hugepages». Он оптимизирует кеширование трансляции виртуальных адресов в физические тем, что одной трансляцией можно получить указатель на страницу физической памяти длиной не восемь килобайт, а два мегабайта или один гигабайт.

### Доступные режимы

«DPDK» по умолчанию организовывает ожидание пакетов с помощью активного ожидания, то есть непрерывного опроса регисторов устройства. Такой метод ожидания сетевых пакетов сильно нагружает ядро. Поэтому в системе доступна работа с прерываниями PCI-устройства.

Кроме этого система использует различные функции для работы с механизмами «VFIO» и «UIO», но это не влияет на организацию захвата/отправки сетевых пакетов при использовании «DPDK» из-за хорошего уровня абстракции.

### Процесс передачи сетевых пакетов

Так как система «DPDK» используется драйвера сетевых устройств в пользовательском пространстве, необходимость во множественном копировании одного пакета при захвате или отправке отсутствует.

При инициализиации работы программы выделяется участок «Hugepage» памяти с фиксированной длиной, информация о котором хранится в структуре `rte_mempool` [4]. Участок делится на фрагменты, указатели на которые хранятся в структурах `rte_mbuf`. Эта структура выполняет функцию передачи пакетов при захвате и отправке сетевых пакетов аналогично структуре `sk_buff` в ядре «Linux».

Для создания структуры `rte_mempool` и выделения памяти выполняются следующие шаги:

1. создание пустой структуры `rte_mempool`:
   1. получение списка всех структур `rte_mempool`;
   2. установка флага о недопустимости использования для ввода-вывода;
   3. получение размера участков памяти;
   4. выделение элемента списка структур `rte_mempool`;
   5. выделение памяти для структуры `rte_mempool`;
   6. инициализация структуры `rte_mempool`;
   7. установка режима работы памяти в структуре `rte_mempool`;
   8. установка указателей на локальный кеш доступных процессоров;
   9. сохранение новой памяти в структуре элемента списка структур `rte_mempool`;
2. опредение операций над страницами памяти и их установка;
3. пользовательская инициализация структуры `rte_mempool`;
4. заполнение структуры `rte_mempool` страницами памяти:
   1. создание операций для работы со страницами памяти;
   2. создание страниц памяти:
      1. получение размера страницы согласно операции подсчёта размера;
      2. создание идентификатора страницы памяти;
      3. установка флага возможности использования для ввода-вывода;
      4. выравнивание страницы памяти;
      5. инициализация страницы памяти для использования для ввода-вывода;
5. инициализация структур `tre_mbuf` в памяти `rte_mempool`.

```c
// contrib/dpdk/lib/mbuf/rte_mbuf.c
// Пример функции выделения памяти и создания структуры `rte_mempool`.
struct rte_mempool *
rte_pktmbuf_pool_create_by_ops(const char *name, unsigned int n,
	unsigned int cache_size, uint16_t priv_size, uint16_t data_room_size,
	int socket_id, const char *ops_name)
{
	struct rte_mempool *mp;
	struct rte_pktmbuf_pool_private mbp_priv;
	const char *mp_ops_name = ops_name;
	unsigned elt_size;
	int ret;

	/* ... */

	// Создание пустой структуры `rte_mempool`.
	mp = rte_mempool_create_empty(name, n, elt_size, cache_size,
		 sizeof(struct rte_pktmbuf_pool_private), socket_id, 0);
	if (mp == NULL)
		return NULL;

	// Опредение операций над страницами памяти.
	if (mp_ops_name == NULL)
		mp_ops_name = rte_mbuf_best_mempool_ops();
	// Установка операций в структуру `rte_mempool`.
	ret = rte_mempool_set_ops_byname(mp, mp_ops_name, NULL);
	if (ret != 0) {
		MBUF_LOG(ERR, "error setting mempool handler");
		rte_mempool_free(mp);
		rte_errno = -ret;
		return NULL;
	}
	// Пользовательская инициализация структуры `rte_mempool`.
	rte_pktmbuf_pool_init(mp, &mbp_priv);

	// Заполнение структуры `rte_mempool` страницами памяти.
	ret = rte_mempool_populate_default(mp);
	if (ret < 0) {
		rte_mempool_free(mp);
		rte_errno = -ret;
		return NULL;
	}

	// Инициализация структур `tre_mbuf` в памяти `rte_mempool`.
	rte_mempool_obj_iter(mp, rte_pktmbuf_init, NULL);

	return mp;
}

// Пример функции-обертки для создания структуры `rte_mempool` и
// выделения памяти.
struct rte_mempool *
rte_pktmbuf_pool_create(const char *name, unsigned int n,
	unsigned int cache_size, uint16_t priv_size, uint16_t data_room_size,
	int socket_id)
{
	return rte_pktmbuf_pool_create_by_ops(name, n, cache_size, priv_size,
			data_room_size, socket_id, NULL);
}
```

```c
// contrib/dpdk/lib/mempool/rte_mempool.c
// Пример функции создания структуры `rte_mempool`.
struct rte_mempool *
rte_mempool_create_empty(const char *name, unsigned n, unsigned elt_size,
	unsigned cache_size, unsigned private_data_size,
	int socket_id, unsigned flags)
{
	char mz_name[RTE_MEMZONE_NAMESIZE];
	struct rte_mempool_list *mempool_list;
	struct rte_mempool *mp = NULL;
	struct rte_tailq_entry *te = NULL;
	const struct rte_memzone *mz = NULL;
	size_t mempool_size;
	unsigned int mz_flags = RTE_MEMZONE_1GB|RTE_MEMZONE_SIZE_HINT_ONLY;
	struct rte_mempool_objsz objsz;
	unsigned lcore_id;
	int ret;

	/* ... */

	// Получение списка всех структур `rte_mempool`.
	mempool_list = RTE_TAILQ_CAST(rte_mempool_tailq.head, rte_mempool_list);

	/* ... */

	// Установка флага о недопустимости использования для ввода-вывода,
	// так как память пуста.
	flags |= RTE_MEMPOOL_F_NON_IO;

	/* ... */

	// Получение размера участков памяти.
	if (!rte_mempool_calc_obj_size(elt_size, flags, &objsz)) {
		rte_errno = EINVAL;
		return NULL;
	}

	// Защита от изменения структур `rte_mempool`.
	rte_mcfg_mempool_write_lock();

	/* ... */

	// Выделение элемента списка структур `rte_mempool`.
	te = rte_zmalloc("MEMPOOL_TAILQ_ENTRY", sizeof(*te), 0);
	if (te == NULL) {
		RTE_MEMPOOL_LOG(ERR, "Cannot allocate tailq entry!");
		goto exit_unlock;
	}

	/* ... */

	// Выделение памяти для структуры `rte_mempool`.
	mz = rte_memzone_reserve(mz_name, mempool_size, socket_id, mz_flags);
	if (mz == NULL)
		goto exit_unlock;

	// Инициализация структуры `rte_mempool`.
	mp = mz->addr;
	memset(mp, 0, RTE_MEMPOOL_HEADER_SIZE(mp, cache_size));
	ret = strlcpy(mp->name, name, sizeof(mp->name));
	if (ret < 0 || ret >= (int)sizeof(mp->name)) {
		rte_errno = ENAMETOOLONG;
		goto exit_unlock;
	}
	mp->mz = mz;
	mp->size = n;
	mp->flags = flags;
	mp->socket_id = socket_id;
	mp->elt_size = objsz.elt_size;
	mp->header_size = objsz.header_size;
	mp->trailer_size = objsz.trailer_size;
	mp->cache_size = cache_size;
	mp->private_data_size = private_data_size;
	STAILQ_INIT(&mp->elt_list);
	STAILQ_INIT(&mp->mem_list);

	// Установка режима работы памяти в структуре `rte_mempool`.
	if ((flags & RTE_MEMPOOL_F_SP_PUT) && (flags & RTE_MEMPOOL_F_SC_GET))
		ret = rte_mempool_set_ops_byname(mp, "ring_sp_sc", NULL);
	else if (flags & RTE_MEMPOOL_F_SP_PUT)
		ret = rte_mempool_set_ops_byname(mp, "ring_sp_mc", NULL);
	else if (flags & RTE_MEMPOOL_F_SC_GET)
		ret = rte_mempool_set_ops_byname(mp, "ring_mp_sc", NULL);
	else
		ret = rte_mempool_set_ops_byname(mp, "ring_mp_mc", NULL);

	if (ret) {
		rte_errno = -ret;
		goto exit_unlock;
	}

	// Установка указателей на локальный кеш доступных процессоров.
	mp->local_cache = (struct rte_mempool_cache *)
		RTE_PTR_ADD(mp, RTE_MEMPOOL_HEADER_SIZE(mp, 0));

	if (cache_size != 0) {
		for (lcore_id = 0; lcore_id < RTE_MAX_LCORE; lcore_id++)
			mempool_cache_init(&mp->local_cache[lcore_id],
					   cache_size);
	}

	// Сохранение новой памяти в структуре элемента списка структур `rte_mempool`.
	te->data = mp;

	// Защита от изменения списка структур`rte_mempool`.
	rte_mcfg_tailq_write_lock();
	// Добавление элемента в список структур `rte_mempool`.
	TAILQ_INSERT_TAIL(mempool_list, te, next);
	// Освобождение доступа к списку структур `rte_mempool`.
	rte_mcfg_tailq_write_unlock();
	// Освобождение доступа к структурам `rte_mempool`.
	rte_mcfg_mempool_write_unlock();

	/* ... */

	return mp;

exit_unlock:
	rte_mcfg_mempool_write_unlock();
	rte_free(te);
	rte_mempool_free(mp);
	return NULL;
}
```

```c
// contrib/dpdk/lib/mempool/rte_mempool.c
// Пример функции заполнения структуры `rte_mempool` страницами памяти.
int
rte_mempool_populate_default(struct rte_mempool *mp)
{
	unsigned int mz_flags = RTE_MEMZONE_1GB|RTE_MEMZONE_SIZE_HINT_ONLY;
	char mz_name[RTE_MEMZONE_NAMESIZE];
	const struct rte_memzone *mz;
	ssize_t mem_size;
	size_t align, pg_sz, pg_shift = 0;
	rte_iova_t iova;
	unsigned mz_id, n;
	int ret;
	bool need_iova_contig_obj;
	size_t max_alloc_size = SIZE_MAX;

	// Создание операций для работы со страницами памяти.
	ret = mempool_ops_alloc_once(mp);
	if (ret != 0)
		return ret;

	// Проверка того, что страницы памяти отсутствуют.
	if (mp->nb_mem_chunks != 0)
		return -EEXIST;

	/* ... */

	// Создание страниц памяти.
	for (mz_id = 0, n = mp->size; n > 0; mz_id++, n -= ret) {
		size_t min_chunk_size;

		// Получение размера страницы согласно операции подсчёта размера.
		mem_size = rte_mempool_ops_calc_mem_size(
			mp, n, pg_shift, &min_chunk_size, &align);

		if (mem_size < 0) {
			ret = mem_size;
			goto fail;
		}

		// Создание идентификатора страницы памяти.
		ret = snprintf(mz_name, sizeof(mz_name),
			RTE_MEMPOOL_MZ_FORMAT "_%d", mp->name, mz_id);
		if (ret < 0 || ret >= (int)sizeof(mz_name)) {
			ret = -ENAMETOOLONG;
			goto fail;
		}

		// Установка флага возможности использования для ввода-вывода.
		if (min_chunk_size == (size_t)mem_size)
			mz_flags |= RTE_MEMZONE_IOVA_CONTIG;

		do {
			// Выравнивание страницы памяти.
			mz = rte_memzone_reserve_aligned(mz_name,
				RTE_MIN((size_t)mem_size, max_alloc_size),
				mp->socket_id, mz_flags, align);

			if (mz != NULL || rte_errno != ENOMEM)
				break;

			max_alloc_size = RTE_MIN(max_alloc_size,
						(size_t)mem_size) / 2;
		} while (mz == NULL && max_alloc_size >= min_chunk_size);

		/* ... */

		// Инициализация страницы памяти для использования для ввода-вывода.
		if (pg_sz == 0 || (mz_flags & RTE_MEMZONE_IOVA_CONTIG))
			ret = rte_mempool_populate_iova(mp, mz->addr,
				iova, mz->len,
				rte_mempool_memchunk_mz_free,
				(void *)(uintptr_t)mz);
		else
			ret = rte_mempool_populate_virt(mp, mz->addr,
				mz->len, pg_sz,
				rte_mempool_memchunk_mz_free,
				(void *)(uintptr_t)mz);
		if (ret == 0)
			ret = -ENOBUFS;
		if (ret < 0) {
			rte_memzone_free(mz);
			goto fail;
		}
	}

	/* ... */

	return mp->size;

 fail:
	rte_mempool_free_memchunks(mp);
	return ret;
}
```

Для изучения алгоритмов захвата и отправки сетевых пакетов будет рассмотрен драйвер пользовательского пространства «IGB».

При захвате пакетов сетевое устройство с помощью DMA записывает полученный пакет в память структуры `rte_mempool` пользовательского процесса. Остаётся лишь получить структуры `rte_mbuf`, в которых содержатся пакеты. Для этого используется функция `rte_eth_rx_burst`, которая обращается к драйверу пользовательского пространства для записи в структуры `rte_mbuf` страниц памяти с полученными пакетами. Для этого выполняются следующие шаги:

1. вызов функции `rte_eth_rx_burst`:
   1. получение указателя на операции драйвера пользовательского пространства по индексу сетевого интерфейса;
   2. получение данных очереди `RX` сетевого интерфейса по её индексу;
   3. вызов операции получения пакетов:
      1. заполнение структур `rte_mbuf`:
         1. получение дескриптора пакета из очереди `RX`;
         2. получение структуры `rte_mbuf` из памяти структуры `rte_mempool`;
         3. получение метаданных дескриптора пакета;
         4. инкрементирование индекса дескриптора пакета;
         5. получение структуры `rte_mbuf` с пакетом и запись новой структуры `rte_mbuf` в метаданные дескриптора пакета;
         6. настройка DMA нового адреса памяти из структуры `rte_mbuf`;
         7. запись в дескриптор нового адреса для получения следующего пакета;
         8. инициализация структуры `rte_mbuf` с пакетом;
         9. запись структуры `rte_mbuf` с пакетом в буфер пользователя;
         10. изменение индекса следующего дескриптора пакета;
      2.  освобождение дескрипторов для сетевого устройства после достижения порога `rxq->rx_free_thresh` путём записи в регистр `rdt_reg_addr` индекса следующего дескриптора;
	4. вызов пользовательской функции по добавлению в структуры `rte_mbuf` произвольных данных или по их изменению.


```c
// contrib/dpdk/lib/ethdev/rte_ethdev.h
// Пример функции захвата пакетов.
static inline uint16_t
rte_eth_rx_burst(uint16_t port_id, uint16_t queue_id,
		 struct rte_mbuf **rx_pkts, const uint16_t nb_pkts)
{
	uint16_t nb_rx;
	struct rte_eth_fp_ops *p;
	void *qd;

	/* ... */

	// Получение указателя на операции драйвера пользовательского пространства
	// по индексу сетевого интерфейса.
	p = &rte_eth_fp_ops[port_id];
	// Получение данных очереди `RX` сетевого интерфейса по её индексу.
	qd = p->rxq.data[queue_id];

	/* ... */

	// Вызов операции получения пакетов.
	nb_rx = p->rx_pkt_burst(qd, rx_pkts, nb_pkts);

	// Сохрание в структуре `rte_mbuf` того, что данные прошли через `RX`
	// очередь.
	rte_mbuf_history_mark_bulk(rx_pkts, nb_rx, RTE_MBUF_HISTORY_OP_RX);

	// Вызов пользовательской функции по добавлению в структуры `rte_mbuf`
	// произвольных данных (например, времени захвата) или по их изменению.
#ifdef RTE_ETHDEV_RXTX_CALLBACKS
	{
		void *cb;

		cb = rte_atomic_load_explicit(&p->rxq.clbk[queue_id],
				rte_memory_order_relaxed);
		if (unlikely(cb != NULL))
			nb_rx = rte_eth_call_rx_callbacks(port_id, queue_id,
					rx_pkts, nb_rx, nb_pkts, cb);
	}
#endif

	/* ... */

	return nb_rx;
}
```

```c
// contrib/dpdk/drivers/net/intel/e1000/igb_rxtx.c
// Пример функции захвата пакетов в драйвере пользовательского пространства.
uint16_t
eth_igb_recv_pkts(void *rx_queue, struct rte_mbuf **rx_pkts,
	       uint16_t nb_pkts)
{
	struct igb_rx_queue *rxq;
	volatile union e1000_adv_rx_desc *rx_ring;
	volatile union e1000_adv_rx_desc *rxdp;
	struct igb_rx_entry *sw_ring;
	struct igb_rx_entry *rxe;
	struct rte_mbuf *rxm;
	struct rte_mbuf *nmb;
	union e1000_adv_rx_desc rxd;
	uint64_t dma_addr;
	uint32_t staterr;
	uint32_t hlen_type_rss;
	uint16_t pkt_len;
	uint16_t rx_id;
	uint16_t nb_rx;
	uint16_t nb_hold;
	uint64_t pkt_flags;

	nb_rx = 0;
	nb_hold = 0;
	rxq = rx_queue;
	// Индекс следующего пакета.
	rx_id = rxq->rx_tail;
	// Кольцо дескрипторов пакетов в драйвере.
	rx_ring = rxq->rx_ring;
	// Кольцо метаданных пакетов в драйвере.
	sw_ring = rxq->sw_ring;
	// Заполнение структур `rte_mbuf`.
	while (nb_rx < nb_pkts) {
		// Получение указателя на следующий дескриптор.
		rxdp = &rx_ring[rx_id];
		/* ... */
		// Чтение дескриптора.
		rxd = *rxdp;

		/* ... */

		// Получение структуры `rte_mbuf` из памяти структуры `rte_mempool`.
		nmb = rte_mbuf_raw_alloc(rxq->mb_pool);
		if (nmb == NULL) {
			/* ... */
			rte_eth_devices[rxq->port_id].data->rx_mbuf_alloc_failed++;
			break;
		}

		nb_hold++;
		// Получение метаданных дескриптора пакета.
		rxe = &sw_ring[rx_id];
		// Инкрементирование индекса дескриптора пакета.
		rx_id++;
		if (rx_id == rxq->nb_rx_desc)
			rx_id = 0;

		// Загрузка пакета в кеш процессора.
		rte_igb_prefetch(sw_ring[rx_id].mbuf);

		/* ... */

		// Получение структуры `rte_mbuf` с пакетом.
		rxm = rxe->mbuf;
		// Запись в дескриптор новой структуры `rte_mbuf`.
		rxe->mbuf = nmb;
		// Настройка DMA нового адреса памяти из структуры `rte_mbuf`.
		dma_addr =
			rte_cpu_to_le_64(rte_mbuf_data_iova_default(nmb));
		// Запись в дескриптор нового адреса для записи следующего пакета.
		rxdp->read.hdr_addr = 0;
		rxdp->read.pkt_addr = dma_addr;

		// Инициализация структуры `rte_mbuf` с пакетом.
		pkt_len = (uint16_t) (rte_le_to_cpu_16(rxd.wb.upper.length) -
				      rxq->crc_len);
		rxm->data_off = RTE_PKTMBUF_HEADROOM;
		rte_packet_prefetch((char *)rxm->buf_addr + rxm->data_off);
		rxm->nb_segs = 1;
		rxm->next = NULL;
		rxm->pkt_len = pkt_len;
		rxm->data_len = pkt_len;
		rxm->port = rxq->port_id;

		rxm->hash.rss = rxd.wb.lower.hi_dword.rss;
		hlen_type_rss = rte_le_to_cpu_32(rxd.wb.lower.lo_dword.data);

		if ((staterr & rte_cpu_to_le_32(E1000_RXDEXT_STATERR_LB)) &&
				(rxq->flags & IGB_RXQ_FLAG_LB_BSWAP_VLAN)) {
			rxm->vlan_tci = rte_be_to_cpu_16(rxd.wb.upper.vlan);
		} else {
			rxm->vlan_tci = rte_le_to_cpu_16(rxd.wb.upper.vlan);
		}
		pkt_flags = rx_desc_hlen_type_rss_to_pkt_flags(rxq, hlen_type_rss);
		pkt_flags = pkt_flags | rx_desc_status_to_pkt_flags(staterr);
		pkt_flags = pkt_flags | rx_desc_error_to_pkt_flags(staterr);
		rxm->ol_flags = pkt_flags;
		rxm->packet_type = igb_rxd_pkt_info_to_pkt_type(rxd.wb.lower.
						lo_dword.hs_rss.pkt_info);

		// Запись структуры `rte_mbuf` с пакетом в буфер пользователя.
		rx_pkts[nb_rx++] = rxm;
	}
	// Изменение индекса следующего пакета.
	rxq->rx_tail = rx_id;

	// Освобождение дескрипторов для сетевого устройства
	// после достижения порога `rxq->rx_free_thresh`
	// путём записи в регистр `rdt_reg_addr` индекса следующего
	// дескриптора для получения следующих пакетов.
	nb_hold = (uint16_t) (nb_hold + rxq->nb_rx_hold);
	if (nb_hold > rxq->rx_free_thresh) {
		/* ... */
		rx_id = (uint16_t) ((rx_id == 0) ?
				     (rxq->nb_rx_desc - 1) : (rx_id - 1));
		E1000_PCI_REG_WRITE(rxq->rdt_reg_addr, rx_id);
		nb_hold = 0;
	}
	rxq->nb_rx_hold = nb_hold;
	return nb_rx;
}
```

Отправка сетевых пакетов, хранящихся в памяти структур `rte_mbuf`, происходит с помощью функции `rte_eth_tx_burst`, которая вызывает функцию `eth_igb_xmit_pkts` драйвера пользовательского пространства. Для этого выполняются следующие шаги:

1. вызов функции `rte_eth_tx_burst`:
   1. получение указателя на операции драйвера пользовательского пространства по индексу сетевого интерфейса;
   2. получение данных очереди `TX` сетевого интерфейса по её индексу;
   3. вызов пользовательской функции по добавлению в структуры `rte_mbuf` произвольных данных или их изменению;
   4. сохрание в структуре `rte_mbuf` того, что данные прошли через `TX` очередь;
   5. вызов операции отправки пакетов:
      1. отправка переданных пакетов:
         1. получение индекса последнего дескриптора для записи пакета;
         2. обновление индекса последнего дескриптора;
         3. запись в очередь метаданных фрагментов пакета:
            1. получение дескрипторов метаданных и памяти;
            2. освобождение памяти с уже отправленными данными;
            3. установка фрагмента в дескриптор метаданных;
            4. настройка DMA для отправки пакета;
         4. установка флага окончания данных;
      2. установка регистра с индексом последнего пакета для отправки;
      3. изменение индекса следующего дескриптора;
   6. сохрание в структуре `rte_mbuf` не обработанных пакетов того, что очередь `TX` недоступна для отправки пакетов.


```c
// contrib/dpdk/lib/ethdev/rte_ethdev.h
// Пример функции отправки пакетов.
static inline uint16_t
rte_eth_tx_burst(uint16_t port_id, uint16_t queue_id,
		 struct rte_mbuf **tx_pkts, uint16_t nb_pkts)
{
	struct rte_eth_fp_ops *p;
	void *qd;

	/* ... */

	// Получение указателя на операции драйвера пользовательского пространства
	// по индексу сетевого интерфейса.
	p = &rte_eth_fp_ops[port_id];
	// Получение данных очереди `TX` сетевого интерфейса по её индексу.
	qd = p->txq.data[queue_id];

	/* ... */

	// Вызов пользовательской функции по добавлению в структуры `rte_mbuf`
	// произвольных данных или их изменению.
#ifdef RTE_ETHDEV_RXTX_CALLBACKS
	{
		void *cb;

		cb = rte_atomic_load_explicit(&p->txq.clbk[queue_id],
				rte_memory_order_relaxed);
		if (unlikely(cb != NULL))
			nb_pkts = rte_eth_call_tx_callbacks(port_id, queue_id,
					tx_pkts, nb_pkts, cb);
	}
#endif

	uint16_t requested_pkts = nb_pkts;
	// Сохрание в структуре `rte_mbuf` того, что данные прошли через `TX`
	// очередь.
	rte_mbuf_history_mark_bulk(tx_pkts, nb_pkts, RTE_MBUF_HISTORY_OP_TX);

	// Вызов операции отправки пакетов.
	nb_pkts = p->tx_pkt_burst(qd, tx_pkts, nb_pkts);

	// Сохрание в структуре `rte_mbuf` того, что очередь `TX` недоступна
	// для отправки пакетов.
	if (requested_pkts > nb_pkts)
		rte_mbuf_history_mark_bulk(tx_pkts + nb_pkts,
				requested_pkts - nb_pkts, RTE_MBUF_HISTORY_OP_TX_BUSY);

	/* ... */
	return nb_pkts;
}
```

```c
// contrib/dpdk/drivers/net/intel/e1000/igb_rxtx.c
// Пример функции отправки пакетов в драйвере пользовательского пространства.

uint16_t
eth_igb_xmit_pkts(void *tx_queue, struct rte_mbuf **tx_pkts,
	       uint16_t nb_pkts)
{
	struct igb_tx_queue *txq;
	struct igb_tx_entry *sw_ring;
	struct igb_tx_entry *txe, *txn;
	volatile union e1000_adv_tx_desc *txr;
	volatile union e1000_adv_tx_desc *txd;
	struct rte_mbuf     *tx_pkt;
	struct rte_mbuf     *m_seg;
	uint64_t buf_dma_addr;
	uint32_t olinfo_status;
	uint32_t cmd_type_len;
	uint32_t pkt_len;
	uint16_t slen;
	uint64_t ol_flags;
	uint16_t tx_end;
	uint16_t tx_id;
	uint16_t tx_last;
	uint16_t nb_tx;
	uint64_t tx_ol_req;
	uint32_t new_ctx = 0;
	uint32_t ctx = 0;
	union igb_tx_offload tx_offload = {0};
	uint64_t ts;

	txq = tx_queue;
	sw_ring = txq->sw_ring;
	txr     = txq->tx_ring;
	// Индекс следующего пакета для отправки.
	tx_id   = txq->tx_tail;
	// Метаданные пакета в драйвере.
	txe = &sw_ring[tx_id];

	// Отправка переданных пакетов.
	for (nb_tx = 0; nb_tx < nb_pkts; nb_tx++) {
		tx_pkt = *tx_pkts++;
		pkt_len = tx_pkt->pkt_len;

		// Загрузка пакета в кеш процессора.
		RTE_MBUF_PREFETCH_TO_FREE(txe->mbuf);

		// Получение индекса последнего дескриптора для записи пакета.
		tx_last = (uint16_t) (tx_id + tx_pkt->nb_segs - 1);

		ol_flags = tx_pkt->ol_flags;
		tx_ol_req = ol_flags & IGB_TX_OFFLOAD_MASK;

		/* ... */

		// Обновление индекса последнего дескриптора.
		if (tx_last >= txq->nb_tx_desc)
			tx_last = (uint16_t) (tx_last - txq->nb_tx_desc);

		/* ... */

		// Запись в очередь метаданных фрагментов пакета.
		m_seg = tx_pkt;
		do {
			// Получение дескрипторов метаданных и памяти.
			txn = &sw_ring[txe->next_id];
			txd = &txr[tx_id];

			// Освобождение памяти с уже отправленными данными.
			if (txe->mbuf != NULL)
				rte_pktmbuf_free_seg(txe->mbuf);
			// Установка фрагмента в дескриптор метаданных;
			txe->mbuf = m_seg;

			// Настройка DMA для отправки пакета.
			slen = (uint16_t) m_seg->data_len;
			buf_dma_addr = rte_mbuf_data_iova(m_seg);
			txd->read.buffer_addr =
				rte_cpu_to_le_64(buf_dma_addr);
			txd->read.cmd_type_len =
				rte_cpu_to_le_32(cmd_type_len | slen);
			txd->read.olinfo_status =
				rte_cpu_to_le_32(olinfo_status);
			txe->last_id = tx_last;
			tx_id = txe->next_id;
			txe = txn;
			m_seg = m_seg->next;
		} while (m_seg != NULL);

		// Установка флага окончания данных.
		txd->read.cmd_type_len |=
			rte_cpu_to_le_32(E1000_TXD_CMD_EOP | E1000_TXD_CMD_RS);
	}
 end_of_tx:
	rte_wmb();

	// Установка регистра с индексом последнего пакета для отправки.
	E1000_PCI_REG_WRITE_RELAXED(txq->tdt_reg_addr, tx_id);
	/* ... */
	// Изменение индекса следующего дескриптора.
	txq->tx_tail = tx_id;

	return nb_tx;
}
```

## Разбор примеров

Перед тем, как запустить примеры необходимо выполнить следующие команды:

```sh
# Загружает в систему драйвер vfio.
modprobe vfio
# Загружает в систему драйвер vfio-pci.
modprobe vfio-pci
# Загружает в систему драйвер uio_pci_generic.
modprobe uio_pci_generic
# Создание в оперативной памяти 1024 страниц размером 2 мегабайта.
echo 1024 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
# Отключает сетевой интерфейс с именем IFACE_NAME.
ip link set down dev IFACE_NAME
# Передаёт управление над сетевым интерфейсом драйверу uio_pci_generic.
dpdk-devbind.py -b uio_pci_generic IFACE_NAME
```

## Настройка сетевого интерфейса

Настройка сетевого интерфейса происходит в несколько этапов:

1. инициализация подсистемы EAL (Environment Abstraction Layer);
2. создание структуры `rte_mempool` и выделение памяти;
3. получение информации о сетевом интерфейсе;
4. настройка колец `RX` и `TX` сетевого интерфейса;
5. запуск сетевого интерфейса;
6. перевод интерфейса в режим "прослушивания".


```c
// src/dpdk/test.c
// Пример функций настройка сетевого интерфейса.

// Функция инициализация сетевого интерфейса (порта).
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

	/* ... */

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

	/* ... */

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

/* ... */

int
main(int argc, char *argv[]) {
	struct rte_mempool *mbuf_pool;
	unsigned nb_ports;

	/* ... */

	// Инициализация подсистемы EAL (Environment Abstraction Layer).
	// Подробнее: https://doc.dpdk.org/api/rte__eal_8h.html#a5c3f4dddc25e38c5a186ecd8a69260e3
	int ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Error: EAL initialization failed\n");

	/* ... */


	// Создание именованного кольца памяти.
	// Подробнее: https://doc.dpdk.org/api/rte__mbuf_8h.html#a8f4abb0d54753d2fde515f35c1ba402a
	mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS,
		MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());

	if (mbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Error: cannot create mbuf pool\n");

	// Инициализация сетевого интерфейса.
	if (port_init(opt_port_id, mbuf_pool) != 0)
		rte_exit(EXIT_FAILURE, "Error: сannot init port %"PRIu16 "\n", opt_port_id);

	/* ... */

	return 0;
}

```

### Захват и отправка сетевых пакетов

Процесс захвата и отправки сетевых пакетов происходит через функции `rte_eth_rx_burst` и `rte_eth_tx_burst`.

```c
// src/dpdk/test.c
// Пример функции захвата или отправки пакетов.
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
```

<!--
```sh
apt install meson ninja-build python3-pyelftools libnuma-dev pkg-config libssl-dev
meson setup build \
	-Dexamples=skeleton \
	-Dmachine=native \
	-Doptimization=2 \
	-Ddebug=true
cd build
ninja
```
 -->

## Полезные материалы

1. [DPDK: принципы разработки высокопроизводительных сетевых приложений (Андрей Новохатько, DINS)](https://www.youtube.com/watch?v=FSQJFAcIWdU)
2. [Linux kernel, DPDK и kernel bypass (Степан Репин)](https://www.youtube.com/watch?v=RMnBHbfZShk&t=484s)

## Источники

1. [Документация ядра «Linux» о механизме «UIO»](https://www.kernel.org/doc/html/latest/driver-api/uio-howto.html)
2. [Документация ядра «Linux» о механизме «VFIO»](https://www.kernel.org/doc/html/latest/driver-api/vfio-mediated-device.html)
3. [Документация ядра «Linux» о механизме «Hugepages»](https://www.kernel.org/doc/html/latest/admin-guide/mm/hugetlbpage.html)
4. [Документация системы «DPDK»](https://doc.dpdk.org/api)