# Песочница

***

В данном разделе описана установка системы виртуализации «QEMU» и виртуальной машины, на которой производится установка систем захвата трафика и их тестирование через использование виртуального сетевого интерфейса, эмулирующего работу драйвера «IGB», который обеспечивает пропускную способность сетевого трафика до 1 Гб/с [1].

***

1. [Установка компонентов](#установка-компонентов)
2. [Создание вертуальной машины](#создание-виртуальной-машины)
3. [Компиляция и установка ядра](#компиляция-и-установка-ядра)
4. [Полезные материалы](#полезные-материалы)
5. [Источники](#источники)

***

## Установка компонентов

Для установки «QEMU» подойдёт любая операционная система на базе ядра «Linux». Систему «QEMU» можно установить из репозиториев либо скомпилировать из исходного кода. Ниже описан способ установки на операционную систему «Ubuntu 24.04» из репозиториев с настройкой через приложение «Virtual Machine Manager» для упрощения настройки и использования.

Устанавливаем необходимые пакеты:

```sh
# Обновление информации о доступных пакетах.
sudo apt update
# qemu-kvm - пакет, содержащий гипервизор QEMU.
# qemu-utils - пакет, содержащий инструменты для работы с виртуальными дисками.
# virt-manager - пакет, содержащий оболочку для работы библиотеки libvirt.
# libvirt-daemon-system libvirt-clients - пакеты с зависимостями и системными утилитами.
# bridge-utils - пакет, содержащий утилиты для настройки моста.
sudo apt install qemu-kvm qemu-utils virt-manager libvirt-daemon-system libvirt-clients bridge-utils virt-viewer
```

Запуск «Virtual Machine Manager» можно производить через графический интерфейс или через командную строку:

```sh
sudo virt-manager
```

## Создание виртуальной машины

Чтобы создать виртуальную машину в «Virtual Machine Manager» можно использовать как графический интерфейс, так и консоль. Производим создание и настройку виртуальной машины через консоль:

```sh
# Проверяет работу службы виртуализации libvirtd.
systemctl status libvirtd
# Создаём директорию для образа виртуальной машины.
sudo mkdir -p /var/lib/libvirt/{images,networks}
sudo chown -R root:libvirt /var/lib/libvirt
sudo chmod -R 775 /var/lib/libvirt
# Перед запуском следующей команды загрузите файл по по пути.
PATH_TO_DEBIAN='/tmp/debian-13.1.0-amd64-netinst.iso'
# Описываем изолированную сеть.
sudo bash -c  "cat >> /var/lib/libvirt/networks/deb13-fp-isolated-network.xml << EOF
<network>
  <name>vm-to-vm</name>
  <forward mode='none'/>
  <bridge name='virbr-vm2vm' stp='on' delay='0'/>
  <ip address='192.168.100.1' netmask='255.255.255.0'>
    <dhcp>
      <range start='192.168.100.100' end='192.168.100.200'/>
    </dhcp>
  </ip>
</network>
EOF
"
# Определяем сеть по XML файлу.
sudo virsh net-define /var/lib/libvirt/networks/deb13-fp-isolated-network.xml
# Запускаем сеть.
sudo virsh net-start vm-to-vm
sudo virsh net-start default
# Настраиваем автозапуск.
sudo virsh net-autostart vm-to-vm
sudo virsh net-autostart default
# virt-install - утилита для создания виртуальной машины.
# --name - имя виртуальной машины.
# --vcpus - количество доступных ядер.
# --memory - размер оперативной памяти в MB.
# --disk - конфигурация накопителя (дисковой памяти нужно будет много).
# --os-variant - название операционной системы.
# --cdrom - конфигурация дисковода.
# --network - конфигурация сетевого адаптера.
# --graphics - конфигурация вывода.
sudo virt-install \
  --name=deb13-fast-packets \
  --vcpus=4 \
  --memory=2048 \
  --disk size=70,path=/var/lib/libvirt/images/deb13-fast-packets.qcow2 \
  --os-variant=debian13 \
  --cdrom=$PATH_TO_DEBIAN \
  --network bridge=virbr0,model=e1000e \
  --network network=vm-to-vm,model=igb \
  --graphics vnc
```

Далее производим стандартную установку операционной системы с созданием пользователя test (графическая оболочка будет не нужна).

Если на сетевых интерфейсах не будет указан IP адрес, то будет необходимо внести изменения в файл на виртуальной машине по пути `/etc/network/interfaces`.

```
# /etc/network/interfaces

source /etc/network/interfaces.d/*

auto lo
iface lo inet loopback

allow-hotplug <Название интерфейса>
iface <Название интерфейса> inet dhcp
```

Для управления виртуальной машиной можно использовать следующие команды:

```sh
# Запуск
sudo virsh start deb13-fast-packets
# Завершение работы
sudo virsh shutdown deb13-fast-packets
# Перезагрузка
sudo virsh reboot deb13-fast-packets
# Вывод адреса виртуальной машины
ip neigh | grep virbr0 | awk '{print $1}'
# Подключение
ssh test@$(ip neigh | grep virbr0 | awk '{print $1}')
```

Все последующие команды предназначены для выполнения в виртуальной машине, если не указано иное. Также предполагается выполнение команд от пользователя root, так как загрузка и выгрузка модулей ядра требует максимальных прав доступа. Для смены пользователя на root выполняем:

```sh
su root
```

## Компиляция и установка ядра

Процесс компиляции ядра и его установка не является сложным и запутанным. В первую очередь, в операционной системе должны быть установлены заголовки используемой версии ядра «Linux» и инструменты для компиляции. Если их нет, то устанавливаем их командой:

```sh
# Установка программ и библиотек для компиляции модулей и ядра
apt update && apt install linux-headers-$(uname -r) build-essential flex bison xz-utils libssl-dev libelf-dev bc
```

После загружаем исходный код ядра и компилируем драйвер:

```sh
# На основной машине выполняем копирование исходников драйвера
scp -r contrib/linux-6.18 test@$(ip neigh | grep virbr0 | awk '{print $1}'):/home/test
```

```sh
# Возвращаемся на виртуальную машину и выполняем компиляцию
cd linux-6.18
# Копируем последнюю конфигурацию ядра
cp $(ls /boot/config-* | sort -V | tail -1) ./.config
# Выполняем обновление конфигурации значениями по умолчанию.
make olddefconfig
# Проверяем, что следующие значения равны `y` или `m`:
# CONFIG_IGB, CONFIG_VFIO, CONFIG_VFIO_PCI, CONFIG_XDP_SOCKETS,
# CONFIG_BPF_JIT, CONFIG_HAVE_EBPF_JIT, CONFIG_HUGETLB_PAGE.
# Иначе устанавливаем их.
cat ./.config  | grep -E 'VFIO|XDP|IGB'
# Компиляция ядра, где аргументом -j является количество доступных процессоров.
# Может занять долгое время.
make -j 4
# Установка ядра.
make install
# Перезагрука системы.
```
Также при работе через `ssh` часть команд может быть недоступна. Тогда нужно добавить директорию `/usr/sbin` в переменную `PATH`.

```sh
# Добавление /usr/sbin в переменную PATH
export PATH=$PATH:/usr/sbin
```

<!-- ## Установка Xdptool

## Установка DPDK -->

## Полезные материалы

- [Документация по компиляции внешних модулей ядра](https://www.kernel.org/doc/html/latest/kbuild/modules.html)

## Источники

1. [Документация по эмуляции драйвера «IGB» в системе «QEMU»](https://www.qemu.org/docs/master/system/devices/igb.html)

