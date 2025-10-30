# Основы работы сетевого стека ядра Linux

***

В данном разделе разбирается устройство сетевого стека ядра Linux от сетевой карты до сокетов.

1. [Драйвер сетевой карты](#драйвер-сетевой-карты)
	 1. [Инициализация и деинициализация](#инициализация-и-деинициализация)
	 2. [Получение сетевых пакетов](#получение-сетевых-пакетов)
	 3. [Отправка сетевых пакетов](#отправка-сетевых-пакетов)
2. [Сокеты ядра Linux](#сокеты-ядра-linux)
3. [Полезные материалы](#полезные-материалы)
4. [Источники](#источники)

***

## Драйвер сетевой карты

В ядре «Linux» существует только три типа устройств [1]:
 - символьные устройства;
 - блочные устройства;
 - сетевые интерфейсы.

За работу сетевого интерфейса отвечает устройство NIC (Network interface controller) или сетевая карта — это аппаратный компонент, который устанавливается в компьютер или сервер для подключения к локальной сети (LAN) [2]. В эталонной модели OSI сетевая карта отвечает не только за работу физического уровня, определяя способ передачи сетевых пакетов, но и за канальный уровень, управляя получаемыми и отправляемыми кадрами. Для этого устройство выполняет следующие функции:

1. формирование кадров и проверка хеш-сумм;
2. запись кадров в оперативную память;
3. фильтрация трафика по адресам;
4. генерация прерываний;
5. аппаратное ускорение обработки пакетов.

Для управления сетевой картой и обеспечением доступа к передаваемым пакетам используется драйвер сетевой карты. Его реализация зависит от используемой операционной системы, но его выполняемые функции остаются общими:

1. инициализация сетевой карты и управление её параметрами;
2. регистрация устройства в ядре операционной системе;
3. обработка прерываний;
4. запись и чтение кадров.

Ядро Linux является монолитным с поддержкой модулей ядра [2]. Модули ядра могут выполнять различные функции от реализации драйверов (модуль «IGB» [3]) и файловых систем (модуль «BTRFS» [4]) до виртуализации (модуль «KVM» [5]), поэтому далее понятия модуля ядра и драйвера будут одним и тем же. Дальнейшее описание работы драйверов сетевых карт будет основано на реализацииЫ модуля «IGB», так как его работу можно эмулировать в системе виртуализации «QEMU» (см. [создание песочницы](Sandbox-qemu.md)).

### Инициализация и деинициализация

Каждое подключенное устройство, например, по шине PCI (Peripheral component interconnect) или по USB (Universal Serial Bus) имеет два численных индекса [1]:

1. VID (Vendor ID) — численный индекс производителя;
2. PID (Product ID) или DID (Device ID) — численный индекс продукта.

Эта пара индексов отвечает за обнаружение драйвером устройства, с которым он умеет работать. Чтобы понять, с какими устройствами может работать драйвер, необходимо найти список структур «pci_device_id», в котором перечислены пары VID и PID поддерживаемых устройств.

```c
// src/e1000_hw.h
// Пример индексов PID
#define E1000_DEV_ID_I354_BACKPLANE_1GBPS	0x1F40
#define E1000_DEV_ID_I354_SGMII			0x1F41
```

```c
// src/igb_main.c
// Пример списка поддерживаемых устройств драйвера «IGB»
static const struct pci_device_id igb_pci_tbl[] = {
	{ PCI_VDEVICE(INTEL, E1000_DEV_ID_I354_BACKPLANE_1GBPS) },
	{ PCI_VDEVICE(INTEL, E1000_DEV_ID_I354_SGMII) },
	/* ... */
	{ PCI_VDEVICE(INTEL, E1000_DEV_ID_82575EB_FIBER_SERDES) },
	{ PCI_VDEVICE(INTEL, E1000_DEV_ID_82575GB_QUAD_COPPER) },
	/* required last entry */
	{0, }
};

// Регистрация списка в системе через файл
// /lib/modules/$(uname -r)/modules.alias
MODULE_DEVICE_TABLE(pci, igb_pci_tbl);
```

После загрузки модуля, например, c помощью команд `insmod` или `modprobe` выполняется функция, которая передается в макрос `module_init`.

```c
// src/igb_main.c
// Структура с информацией о модуле
static struct pci_driver igb_driver = {
	// Имя модуля
	.name     = igb_driver_name,
	// Поддерживаемые устройства
	.id_table = igb_pci_tbl,
	// Функция регистрация устройства
	.probe    = igb_probe,
	// Функция удаления устройства
	.remove   = __devexit_p(igb_remove),
	// Функция приостановки устройства
	.suspend  = igb_suspend,
	// Функция возобновления работы устройства
	.resume   = igb_resume,
	// Возможны и другие функции
	// в зависимости от конфигурации
	/* ... */
};

// Пример инициализации модуля
static int __init igb_init_module(void)
{
	/* ... */
	// Установка структуры с данными драйвера
	ret = pci_register_driver(&igb_driver);
	/* ... */
}

// Регистрация функции инициализации
module_init(igb_init_module);
```

После того, как модуль будет инициализирован, запустится функция `igb_probe` (`igb_driver.probe`) для каждого поддерживаемого устройства, которая выполнит их инициализацию. Для драйвера «IGB» устройствами будут являться сетевые интерфейсы. Их инициализация состоит из следующих шагов:

1. Инициализация PCI-устройства [6];
2. Установка маски DMA [7];
3. Резервирование участков памяти [6];
4. Захват шины PCI для управления устройством [6];
5. Создание и заполнение структуры «net_device» для регистрации сетевого интерфейса [7];
6. Регистрирация поддерживаемых функций ethtool (см. [настройка и тестирование](Settings-and-testing.md));
7. Настройка прерываний и подсистемы NAPI [8]
8. Множество других настроек в зависимости от конфигурации и устройства

```c
// src/igb/igb_main.c
// Структура с операциями над сетевым интерфейсом
static const struct net_device_ops igb_netdev_ops = {
	.ndo_open		= igb_open,
	.ndo_stop		= igb_close,
	.ndo_start_xmit		= igb_xmit_frame,
	.ndo_get_stats		= igb_get_stats,
	.ndo_set_rx_mode	= igb_set_rx_mode,
	.ndo_set_mac_address	= igb_set_mac,
	/* ... */
};
```

```c
// src/igb/igb_ethtool.c
// Структура с операциями над сетевым интерфейсом при помощи ethtool
static const struct ethtool_ops igb_ethtool_ops = {
	/* ... */
	.get_drvinfo            = igb_get_drvinfo,
	.get_regs_len           = igb_get_regs_len,
	.get_regs               = igb_get_regs,
	.get_wol                = igb_get_wol,
	.set_wol                = igb_set_wol,
	.get_msglevel           = igb_get_msglevel,
	.set_msglevel           = igb_set_msglevel,
	.nway_reset             = igb_nway_reset,
	.get_link               = igb_get_link,
	.get_eeprom_len         = igb_get_eeprom_len,
	.get_eeprom             = igb_get_eeprom,
	.set_eeprom             = igb_set_eeprom,
	.get_ringparam          = igb_get_ringparam,
	.set_ringparam          = igb_set_ringparam,
	.get_pauseparam         = igb_get_pauseparam,
	.set_pauseparam         = igb_set_pauseparam,
	.self_test              = igb_diag_test,
	.get_strings            = igb_get_strings,
	/* ... */
};
```

```c
// src/igb/igb_main.c
// Пример инициализации устройства
static int igb_probe(struct pci_dev *pdev,
			       const struct pci_device_id *ent)
{
	/* ... */
	struct net_device *netdev;
	struct igb_adapter *adapter;
	int err;
	/* ... */
	// Инициализация PCI-устройства
	err = pci_enable_device_mem(pdev);
	if (err)
		return err;
	
	// Установка маски DMA
	err = dma_set_mask(pci_dev_to_dev(pdev), DMA_BIT_MASK(64));
	if (!err) {
		err = dma_set_coherent_mask(pci_dev_to_dev(pdev),
			DMA_BIT_MASK(64));
		if (!err)
			pci_using_dac = 1;
	} else {
		err = dma_set_mask(pci_dev_to_dev(pdev), DMA_BIT_MASK(32));
		if (err) {
			err = dma_set_coherent_mask(pci_dev_to_dev(pdev),
				DMA_BIT_MASK(32));
			if (err) {
				IGB_ERR(
				  "No usable DMA configuration, aborting\n");
				goto err_dma;
			}
		}
	}
	
	/* ... */
	
	// Резервирование участков памяти
	err = pci_request_selected_regions(pdev,
					  pci_select_bars(pdev,
							  IORESOURCE_MEM),
					  igb_driver_name);
	if (err)
		goto err_pci_reg;

	/* ... */
	
	// Захват шины PCI для управления устройством
	pci_set_master(pdev);

	/* ... */
	
	// Создание и заполнение структуры net_device для регистрации сетевого интерфейса
	netdev = alloc_etherdev_mq(sizeof(struct igb_adapter),
				   IGB_MAX_TX_QUEUES);
	/* ... */

	if (!netdev)
		goto err_alloc_etherdev;

	SET_MODULE_OWNER(netdev);
	SET_NETDEV_DEV(netdev, &pdev->dev);

	pci_set_drvdata(pdev, netdev);
	adapter = netdev_priv(netdev);
	adapter->netdev = netdev;
	adapter->pdev = pdev;
	hw = &adapter->hw;
	hw->back = adapter;
	adapter->port_num = hw->bus.func;
	adapter->msg_enable = GENMASK(debug - 1, 0);

	/* ... */

#ifdef HAVE_NET_DEVICE_OPS
	netdev->netdev_ops = &igb_netdev_ops;
#endif /* HAVE_NET_DEVICE_OPS */

	// Регистрирация поддерживаемых функций ethtool
	igb_set_ethtool_ops(netdev);

	/* ... */

	strscpy(netdev->name, pci_name(pdev), sizeof(netdev->name));

	/* ... */
	
	// Настройка прерываний и подсистемы NAPI
	err = igb_sw_init(adapter);
	if (err)
		goto err_sw_init;
	
	/* ... */
}
```

Далее с помощью функций из структур `net_device_ops` и `ethtool_ops` происходит настройка сетевого интерфейса из пространства пользователя.

При окончании работы сетевого драйвера для каждого интерфейса вызывается функция `igb_remove` (`igb_driver.remove`). Её выполнение состоит из следующих шагов:

1. ...
2. ...
3. ...

```c
```

Далее рассмотрим технологии, которые применяются при работе сетевой карты.

#### DMA

DMA (Direct Memory Access) — это технология, позволяющая устройствам ввода/вывода читать и записывать данные в оперативную память напрямую, без участия центрального процессора (CPU).

Для настройки работы DMA используются функции «dma_set_mask», которая настраивает маску для потоковых DMA-операций (одиночные передачи), «dma_set_coherent_mask», которая настраивает маску для когерентных DMA-операций (постоянные отображения памяти), или «dma_set_mask_and_coherent», которая настраивает маску и для потоковых, и для когерентных DMA-операций [8].

(Описать картинку отображения памяти).

### NAPI


(Устройства на шине PCI [2])
(Драйверы: сетевой интерфейс [2])
(Из исходного кода/module_deinit)

### Получение сетевых пакетов

(https://blog.packagecloud.io/monitoring-tuning-linux-networking-stack-receiving-data/)
(https://habr.com/ru/companies/vk/articles/314168/)

### Отправка сетевых пакетов

(https://blog.packagecloud.io/monitoring-tuning-linux-networking-stack-sending-data/)

## Сокеты ядра Linux

(https://man7.org/linux/man-pages/man7/socket.7.html)
(https://habr.com/ru/articles/886058/)

## Полезные материалы

- [Стандарт ISO/IEC 7498](https://ecma-international.org/wp-content/uploads/s020269e.pdf) или [ГОСТ Р ИСО/МЭК 7498-1-99](https://internet-law.ru/gosts/gost/4269/).
- [How To Write Linux PCI Drivers](https://github.com/torvalds/linux/blob/master/Documentation/PCI/pci.rst)

## Источники

1. Цилюрик О. И. Расширения ядра Linux: драйверы и модули. — СПб.: БХВ-Петербург, 688 с.: ил. ISBN 978-5-9775-1719-5
2. [Определение сетевой карты](https://www.gartner.com/en/information-technology/glossary/nic-network-interface-card)
3. [Документация ядра «Linux» о модуле «IGB»](https://www.kernel.org/doc/html/latest/networking/device_drivers/ethernet/intel/igb.html)
4. [Документация ядра «Linux» о модуле «BTRFS»](https://www.kernel.org/doc/html/latest/filesystems/btrfs.html)
5. [Документация ядра «Linux» о модуле «KVM»](https://www.kernel.org/doc/html/latest/virt/kvm/api.html)
6. [Документация ядра «Linux» о работе с PCI](https://www.kernel.org/doc/html/next/driver-api/pci/pci.html)
7. [Документация ядра «Linux» о работе с сетевыми интерфейсами](https://www.kernel.org/doc/html/latest/networking/kapi.html)
8. [Документация ядра «Linux» о работе c NAPI](https://www.kernel.org/doc/html/latest/networking/napi.html)
9. [Документация ядра «Linux» о работе с DMA](https://www.kernel.org/doc/html/latest/core-api/dma-api-howto.html)
10. 

