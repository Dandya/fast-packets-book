#!/bin/bash

# Завершение работы в случае ненулевого возврата.
set -e

if [ $# -ne 1 ]; then
	echo "Usage: $0 <interface_name>"
	exit 1
fi

INTERFACE="$1"

# Получение номеров прерываний для очередей сетевого интерфейса.
IRQS=$(grep "$INTERFACE" /proc/interrupts | grep -i rx | awk '{print $1}' | sed 's/://')

if [ -z "$IRQS" ]; then
	echo "Error: No IRQs found for interface $INTERFACE"
	exit 1
fi

# Получение количества доступных логических процессоров.
CPU_COUNT=$(nproc --all)
echo "Available CPUs: 0-$((CPU_COUNT-1))"

# Настройка прерываний через файлы smp_affinity_list.
echo -e "\nSetting IRQ affinities:"
INDEX=0
for IRQ in $IRQS; do
	if ! [[ "$IRQ" =~ ^[0-9]+$ ]]; then
		continue
	fi

	PROC_FILE="/proc/irq/$IRQ/smp_affinity_list"

	if [ -f "$PROC_FILE" ]; then
		CPU=$((INDEX % CPU_COUNT))
		OLD_VALUE=$(cat "$PROC_FILE" 2>/dev/null || echo "unknown")
		echo "$CPU" > "$PROC_FILE"
		echo "IRQ $IRQ: CPU $OLD_VALUE -> $CPU"
		INDEX=$((INDEX + 1))
	else
		echo "Warning: /proc/irq/$IRQ/smp_affinity_list not found"
	fi
done

echo -e "\nDone. Summary for $INTERFACE:"
for IRQ in $IRQS; do
	if [[ "$IRQ" =~ ^[0-9]+$ ]]; then
		PROC_FILE="/proc/irq/$IRQ/smp_affinity_list"
		if [ -f "$PROC_FILE" ]; then
			echo -n "IRQ $IRQ: "
			cat "$PROC_FILE"
		fi
	fi
done