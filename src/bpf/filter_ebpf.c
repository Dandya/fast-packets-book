
#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/in.h>
#include <bpf/bpf_helpers.h>

struct pkt_desc {
	__u32 len;
	__u32 caplen;
	__u64 offset_to_next;
	__u8 data[];
};

#define ETHERTYPE_IP 0x0800
#define ETHERTYPE_IPV6 0x86DD
// #define IPPROTO_TCP 6
// #define IPPROTO_UDP 17

static __always_inline __u16 bpf_htons(__u16 x) {
	return ((x & 0xFF00) >> 8) | ((x & 0x00FF) << 8);
}

static __always_inline __u16 bpf_ntohs(__u16 x) {
	return bpf_htons(x); // То же самое преобразование
}

static __always_inline __u32 bpf_htonl(__u32 x) {
	return ((x & 0xFF000000) >> 24) |
			 ((x & 0x00FF0000) >> 8) |
			 ((x & 0x0000FF00) << 8) |
			 ((x & 0x000000FF) << 24);
}

static __always_inline __u32 bpf_ntohl(__u32 x) {
	return bpf_htonl(x); // То же самое преобразование
}

static __always_inline int is_unicast_mac(const unsigned char *mac) {
	return (mac[0] & 0x01) == 0;
}

#pragma pack(push, 1)
struct ether_header_custom {
	__u8	ether_dhost[6];
	__u8	ether_shost[6];
	__u16 ether_type;
};
#pragma pack(pop)

// Вспомогательные функции для доступа к данным с проверкой границ
static __always_inline __u8 check_bounds(const __u8* data, __u64 data_len, __u64 offset, __u64 needed) {
	return (offset + needed <= data_len);
}

static __always_inline __u8 get_u8_safe(const __u8* data, __u64 data_len, __u64 offset) {
	return check_bounds(data, data_len, offset, 1) ? data[offset] : 0;
}

static __always_inline __u16 get_u16_safe(const __u8* data, __u64 data_len, __u64 offset) {
	if (!check_bounds(data, data_len, offset, 2))
		return 0;
	return bpf_ntohs(*(__u16*)(data + offset));
}

static __always_inline __u32 get_u32_safe(const __u8* data, __u64 data_len, __u64 offset) {
	if (!check_bounds(data, data_len, offset, 4))
		return 0;
	return bpf_ntohl(*(__u32*)(data + offset));
}

static __always_inline __u8 is_ipv4_packet(const __u8* data, __u64 len) {
	if (len < sizeof(struct ether_header_custom))
		return 0;
	const struct ether_header_custom* eth = (const struct ether_header_custom*)data;
	return bpf_ntohs(eth->ether_type) == ETHERTYPE_IP;
}

static __always_inline __u8 is_ipv6_packet(const __u8* data, __u64 len) {
	if (len < sizeof(struct ether_header_custom))
		return 0;
	const struct ether_header_custom* eth = (const struct ether_header_custom*)data;
	return bpf_ntohs(eth->ether_type) == ETHERTYPE_IPV6;
}

static __always_inline __u8 check_filter(const struct pkt_desc *desc) {
	const __u8* packet = desc->data;
	__u64 length = desc->caplen;
	if (length < sizeof(struct ether_header_custom))
		return 0;

	const struct ether_header_custom* eth = (const struct ether_header_custom*)packet;
	__u16 ether_type = bpf_ntohs(eth->ether_type);

	if (ether_type != ETHERTYPE_IP && ether_type != ETHERTYPE_IPV6) {
		return 0;
	}

	const __u8* ip_layer = packet + sizeof(struct ether_header_custom);
	__u64 ip_layer_len = length - sizeof(struct ether_header_custom);

	if (ether_type == ETHERTYPE_IP) {
		if (ip_layer_len < sizeof(struct iphdr)) {
			return 0;
		}

		const struct iphdr* iph = (const struct iphdr*)ip_layer;
		__u8 ip_header_len = iph->ihl * 4;
		__u8 proto = iph->protocol;

		if (ip_layer_len < ip_header_len) {
			return 0;
		}

		const __u8* transport_layer = ip_layer + ip_header_len;
		__u64 transport_len = ip_layer_len - ip_header_len;

		if (proto == IPPROTO_TCP && transport_len >= sizeof(struct tcphdr)) {
			const struct tcphdr* tcph = (const struct tcphdr*)transport_layer;
			__u16 src_port = bpf_ntohs(tcph->source);
			__u16 dst_port = bpf_ntohs(tcph->dest);

			// dst port 80 and (tcp[tcpflags] & tcp-syn) != 0
			if (dst_port == 80 && tcph->syn) {
				return 1;
			}

			// dst port 443 and (tcp[tcpflags] & (tcp-syn|tcp-ack)) == (tcp-syn|tcp-ack)
			if (dst_port == 443 && tcph->syn && tcph->ack) {
				return 1;
			}

			// dst port 22 and (tcp[tcpflags] & tcp-syn) != 0
			if (dst_port == 22 && tcph->syn) {
				return 1;
			}

			// dst portrange 10000-20000 and not src port 53
			if (dst_port >= 10000 && dst_port <= 20000 && src_port != 53) {
				return 1;
			}
		}

		// UDP проверки
		else if (proto == IPPROTO_UDP && transport_len >= sizeof(struct udphdr)) {
			const struct udphdr* udph = (const struct udphdr*)transport_layer;
			__u16 src_port = bpf_ntohs(udph->source);
			__u16 dst_port = bpf_ntohs(udph->dest);
			__u16 udp_length = bpf_ntohs(udph->len);

			// dst port 53 and length > 100
			if (dst_port == 53 && length > 100) {
				return 1;
			}

			// dst port 123 and ip[8] == 0x48
			// ip[8] - это TTL поле в IPv4 заголовке (смещение 8 байт от начала IP заголовка)
			if (dst_port == 123 && iph->ttl == 0x48) {
				return 1;
			}

			// src port 67 and dst port 68 and ether[0] & 1 == 0
			// Проверяем что MAC адрес назначения unicast (первый бит = 0)
			if (src_port == 67 && dst_port == 68 && is_unicast_mac(eth->ether_dhost)) {
				return 1;
			}

			// dst port 5060 and udp[20:2] != 0x5349
			// Проверяем 2 байта начиная с 20-го байта UDP payload
			if (dst_port == 5060) {
				// UDP заголовок 8 байт, так что payload начинается с transport_layer + 8
				const __u8* udp_payload = transport_layer + sizeof(struct udphdr);
				__u64 udp_payload_len = transport_len - sizeof(struct udphdr);

				if (udp_payload_len >= 22 - sizeof(struct udphdr)) { // Нужно 20+2 байта
					__u16 value = get_u16_safe(udp_payload, udp_payload_len, 20 - sizeof(struct udphdr));
					if (value != 0x5349) {
						return 1;
					}
				}
			}

			// dst port 1900 and ip[9] == 0x11 and ip[8] == 0x40
			if (dst_port == 1900) {
				// В IPv4 заголовке:
				// ip[8] = TTL (iph->ttl)
				// ip[9] = Protocol (iph->protocol)
				if (iph->protocol == 0x11 && iph->ttl == 0x40) {
					return 1;
				}
			}
		}
	}

	// Обработка IPv6 пакетов (только TCP/UDP проверки, без специфических IPv6 проверок)
	else if (ether_type == ETHERTYPE_IPV6) {
		if (ip_layer_len < sizeof(struct ipv6hdr)) {
			return 0;
		}

		const struct ipv6hdr* ip6h = (const struct ipv6hdr*)ip_layer;
		__u8 next_header = ip6h->nexthdr;

		// Для упрощения предположим, что нет extension headers
		const __u8* transport_layer = ip_layer + sizeof(struct ipv6hdr);
		__u64 transport_len = ip_layer_len - sizeof(struct ipv6hdr);

		// Определяем реальный протокол транспортного уровня
		// В реальности нужно обрабатывать extension headers
		__u8 proto = next_header;

		// TCP проверки для IPv6
		if (proto == IPPROTO_TCP && transport_len >= sizeof(struct tcphdr)) {
			const struct tcphdr* tcph = (const struct tcphdr*)transport_layer;
			__u16 src_port = bpf_ntohs(tcph->source);
			__u16 dst_port = bpf_ntohs(tcph->dest);

			// dst port 80 and (tcp[tcpflags] & tcp-syn) != 0
			if (dst_port == 80 && tcph->syn) {
				return 1;
			}

			// dst port 443 and (tcp[tcpflags] & (tcp-syn|tcp-ack)) == (tcp-syn|tcp-ack)
			if (dst_port == 443 && tcph->syn && tcph->ack) {
				return 1;
			}

			// dst port 22 and (tcp[tcpflags] & tcp-syn) != 0
			if (dst_port == 22 && tcph->syn) {
				return 1;
			}

			// dst portrange 10000-20000 and not src port 53
			if (dst_port >= 10000 && dst_port <= 20000 && src_port != 53) {
				return 1;
			}
		}

		// UDP проверки для IPv6
		else if (proto == IPPROTO_UDP && transport_len >= sizeof(struct udphdr)) {
			const struct udphdr* udph = (const struct udphdr*)transport_layer;
			__u16 src_port = bpf_ntohs(udph->source);
			__u16 dst_port = bpf_ntohs(udph->dest);
			__u16 udp_length = bpf_ntohs(udph->len);

			// dst port 53 and length > 100
			if (dst_port == 53 && udp_length > 100) {
				return 1;
			}

			// Для IPv6 нет TTL поля как в IPv4, пропускаем проверку ip[8]
			// src port 67 and dst port 68 and ether[0] & 1 == 0
			if (src_port == 67 && dst_port == 68 && is_unicast_mac(eth->ether_dhost)) {
				return 1;
			}

			// dst port 5060 and udp[20:2] != 0x5349
			if (dst_port == 5060) {
				const __u8* udp_payload = transport_layer + sizeof(struct udphdr);
				__u64 udp_payload_len = transport_len - sizeof(struct udphdr);

				if (udp_payload_len >= 22) {
					__u16 value = get_u16_safe(udp_payload, udp_payload_len, 20);
					if (value != 0x5349) {
						return 1;
					}
				}
			}

			if (dst_port == 1900 && ip_layer[9] == 0x11 && ip_layer[8] == 0x40) {
				return 1;
			}
		}
	}

	return 0;
}
// Основная функция eBPF.
SEC("UNSPEC")
int filter_ebpf(const struct pkt_desc *desc) {
	return check_filter(desc);
}

char __license[] SEC("license") = "GPL";
