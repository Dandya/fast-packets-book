```sh
apt install meson ninja-build python3-pyelftools libnuma-dev pkg-config libssl-dev
meson setup build \
    -Dexamples=skeleton \
    -Dmachine=native \
    -Doptimization=2 \
    -Ddebug=true
cd build
ninja

# Заёпуск
modprobe vfio
modprobe vfio-pci
modprobe uio_pci_generic
echo 1024 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
ip link set down dev enp3s0
dpdk-devbind.py -b uio_pci_generic enp3s0
dpdk-devbind.py --status
```