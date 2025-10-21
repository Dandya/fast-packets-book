# Песочница

Для знакомства с системами захвата сетевого трафика будет использоваться система виртуализации «QEMU», которая может эмулировать сетевой адаптер для работы драйвера «IGB» [1].

## Установка компонентов

Для установки «QEMU» подойдёт любая операционная система на базе ядра «Linux». Систему «QEMU» можно установить из репозиториев либо скомпилировать из исходного кода. Ниже описан способ установки из репозиториев с настройкой через приложение «Virtual Machine Manager» для упрощения настройки и использования.

Установим необходимые пакеты:

```sh
# Обновление информации о доступных пакетах
sudo apt update
# qemu-kvm - пакет, содержащий гипервизор QEMU
# qemu-utils - пакет, содержащий инструменты для работы с виртуальными дисками
# virt-manager - пакет, содержащий оболочку для работы библиотеки libvirt
# libvirt-daemon-system libvirt-clients - пакеты с зависимостями и системными утилитами
# bridge-utils - пакет, содержащий утилиты для настройки моста
sudo apt install qemu-kvm qemu-utils virt-manager libvirt-daemon-system libvirt-clients bridge-utils
```

После установки необходимо перезапустить систему. Далее запуск «Virtual Machine Manager» можно производить через графический интерфейс или через командную строку:

```sh
sudo virt-manager
```

## Создание виртуальных машин

Чтобы создать виртуальную машину в «Virtual Machine Manager» можно использовать как графический интерфейс, так и консоль. Ниже представлен вариант создания и настройки виртуальной машины через консоль.

```sh
# Проверяет работу службы виртуализации libvirtd
systemctl status libvirtd
# Перед запуском следующей команды загрузите файл по по пути
PATH_TO_DEBIAN='/tmp/debian-13.1.0-amd64-netinst.iso'
# Описываем изолированную сеть
cat >> /home/$USER/Desktop/isolated-network.xml << EOF
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
# Опеределяем сеть по XML файлу
sudo virsh net-define /home/$USER/Desktop/isolated-network.xml
# Запускаем сеть
sudo virsh net-start vm-to-vm
# Настраиваем автозапуск
sudo virsh net-autostart vm-to-vm

# virt-install - утилита для создания виртуальной машины
# --name - имя виртуальной машины
# --vcpus - количество доступных ядер
# --memory - размер оперативной памяти в MB
# --disk - конфигурация накопителя
# --os-variant - название операционной системы
# --cdrom - конфигурация дисковода
# --network - конфигурация сетевого адаптера
# --graphics - конфигурация вывода
sudo virt-install \
  --name=deb13-fast-packets \
  --vcpus=2 \
  --memory=2048 \
  --disk size=30,path=/home/$USER/Desktop/deb13-fast-packets.qcow2 \
  --os-variant=debian13 \
  --cdrom=$PATH_TO_DEBIAN \
  --network bridge=virbr0,model=virtio \
  --network network=vm-to-vm,model=igb \
  --graphics vnc
```

Далее производим стандартную установку системы (графическая оболочка будет не нужна).


## Сборка модуля «IGB»

Модуль «IGB» является драйвером ... и обеспечивает скорость сети до 1 Гб/с (**полнодуплекс**). Для возможности внесения изменений и изучения работы модуля необходимо загрузить его исходный код:

```sh
wget https://github.com/intel/ethernet-linux-igb/releases/download/v5.19.4/igb-5.19.4.tar.gz
tar -xf igb-5.19.4.tar.gz
```

Для сборки модуля необходимо установить следующие пакеты:

```sh
...
```

После этого можно выполнить сборку модуля:

```sh
...
```

## Источники

1. https://www.qemu.org/docs/master/system/devices/igb.html