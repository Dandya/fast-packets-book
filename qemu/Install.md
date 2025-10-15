Порядок установки на debian:

```sh
sudo apt update
sudo apt install qemu-kvm qemu-utils virt-manager libvirt-daemon-system libvirt-clients bridge-utils
```

Чтобы запускать без sudo:

```sh
sudo usermod -a -G libvirt $USER
```

Запуск virt-manager:

```sh
virt-manager
```

Далее создаём виртуалку и настраиваем устройство igb.