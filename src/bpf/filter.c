#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>
#include <net/ethernet.h>
#include <arpa/inet.h>

#pragma pack(push, 1)
struct ether_header_custom {
	uint8_t  ether_dhost[6];
	uint8_t  ether_shost[6];
	uint16_t ether_type;
};
#pragma pack(pop)

// Вспомогательные функции для доступа к данным с проверкой границ
static inline bool check_bounds(const uint8_t* data, size_t data_len, size_t offset, size_t needed) {
	return (offset + needed <= data_len);
}

static inline uint8_t get_u8_safe(const uint8_t* data, size_t data_len, size_t offset) {
	return check_bounds(data, data_len, offset, 1) ? data[offset] : 0;
}

static inline uint16_t get_u16_safe(const uint8_t* data, size_t data_len, size_t offset) {
	if (!check_bounds(data, data_len, offset, 2))
		return 0;
	return ntohs(*(uint16_t*)(data + offset));
}

static inline uint32_t get_u32_safe(const uint8_t* data, size_t data_len, size_t offset) {
	if (!check_bounds(data, data_len, offset, 4))
		return 0;
	return ntohl(*(uint32_t*)(data + offset));
}

static bool is_unicast_mac(const uint8_t* mac) {
	return (mac[0] & 0x01) == 0;
}

static bool is_ipv4_packet(const uint8_t* data, size_t len) {
	if (len < sizeof(struct ether_header_custom))
		return false;
	const struct ether_header_custom* eth = (const struct ether_header_custom*)data;
	return ntohs(eth->ether_type) == ETHERTYPE_IP;
}

static bool is_ipv6_packet(const uint8_t* data, size_t len) {
	if (len < sizeof(struct ether_header_custom))
		return false;
	const struct ether_header_custom* eth = (const struct ether_header_custom*)data;
	return ntohs(eth->ether_type) == ETHERTYPE_IPV6;
}

bool check_filter(const uint8_t* packet, size_t length) {
	if (length < sizeof(struct ether_header_custom))
		return false;

	const struct ether_header_custom* eth = (const struct ether_header_custom*)packet;
	uint16_t ether_type = ntohs(eth->ether_type);

	if (ether_type != ETHERTYPE_IP && ether_type != ETHERTYPE_IPV6) {
		return false;
	}

	const uint8_t* ip_layer = packet + sizeof(struct ether_header_custom);
	size_t ip_layer_len = length - sizeof(struct ether_header_custom);

	if (ether_type == ETHERTYPE_IP) {
		if (ip_layer_len < sizeof(struct ip)) {
			return false;
		}

		const struct ip* iph = (const struct ip*)ip_layer;
		uint8_t ip_header_len = iph->ip_hl * 4;
		uint8_t proto = iph->ip_p;

		if (ip_layer_len < ip_header_len) {
			return false;
		}

		const uint8_t* transport_layer = ip_layer + ip_header_len;
		size_t transport_len = ip_layer_len - ip_header_len;

		if (proto == IPPROTO_TCP && transport_len >= sizeof(struct tcphdr)) {
			const struct tcphdr* tcph = (const struct tcphdr*)transport_layer;
			uint16_t src_port = ntohs(tcph->th_sport);
			uint16_t dst_port = ntohs(tcph->th_dport);
			uint8_t tcp_flags = tcph->th_flags;

			// dst port 80 and (tcp[tcpflags] & tcp-syn) != 0
			if (dst_port == 80 && (tcp_flags & TH_SYN) != 0) {
				return true;
			}

			// dst port 443 and (tcp[tcpflags] & (tcp-syn|tcp-ack)) == (tcp-syn|tcp-ack)
			if (dst_port == 443 && (tcp_flags & (TH_SYN | TH_ACK)) == (TH_SYN | TH_ACK)) {
				return true;
			}

			// dst port 22 and (tcp[tcpflags] & tcp-syn) != 0
			if (dst_port == 22 && (tcp_flags & TH_SYN) != 0) {
				return true;
			}

			// dst portrange 10000-20000 and not src port 53
			if (dst_port >= 10000 && dst_port <= 20000 && src_port != 53) {
				return true;
			}
		}

		// UDP проверки
		else if (proto == IPPROTO_UDP && transport_len >= sizeof(struct udphdr)) {
			const struct udphdr* udph = (const struct udphdr*)transport_layer;
			uint16_t src_port = ntohs(udph->uh_sport);
			uint16_t dst_port = ntohs(udph->uh_dport);
			uint16_t udp_length = ntohs(udph->uh_ulen);

			// dst port 53 and length > 100
			if (dst_port == 53 && udp_length > 100) {
				return true;
			}

			// dst port 123 and ip[8] == 0x48
			// ip[8] - это TTL поле в IPv4 заголовке (смещение 8 байт от начала IP заголовка)
			if (dst_port == 123 && iph->ip_ttl == 0x48) {
				return true;
			}

			// src port 67 and dst port 68 and ether[0] & 1 == 0
			// Проверяем что MAC адрес назначения unicast (первый бит = 0)
			if (src_port == 67 && dst_port == 68 && is_unicast_mac(eth->ether_dhost)) {
				return true;
			}

			// dst port 5060 and udp[20:2] != 0x5349
			// Проверяем 2 байта начиная с 20-го байта UDP payload
			if (dst_port == 5060) {
				// UDP заголовок 8 байт, так что payload начинается с transport_layer + 8
				const uint8_t* udp_payload = transport_layer + sizeof(struct udphdr);
				size_t udp_payload_len = transport_len - sizeof(struct udphdr);

				if (udp_payload_len >= 22) { // Нужно 20+2 байта
					uint16_t value = get_u16_safe(udp_payload, udp_payload_len, 20);
					if (value != 0x5349) {
						return true;
					}
				}
			}

			// dst port 1900 and ip[9] == 0x01 and ip[8] == 0x40
			// ip[9] - protocol (должен быть UDP = 0x11, а не 0x01!)
			// Вероятно, в фильтре ошибка - для UDP должно быть ip[9] == 0x11
			// Но реализуем как в фильтре: проверяем protocol и TTL
			if (dst_port == 1900) {
				// В IPv4 заголовке:
				// ip[8] = TTL (iph->ip_ttl)
				// ip[9] = Protocol (iph->ip_p)
				if (iph->ip_p == 0x01 && iph->ip_ttl == 0x40) {
					return true;
				}
			}
		}
	}

	// Обработка IPv6 пакетов (только TCP/UDP проверки, без специфических IPv6 проверок)
	else if (ether_type == ETHERTYPE_IPV6) {
		if (ip_layer_len < sizeof(struct ip6_hdr)) {
			return false;
		}

		const struct ip6_hdr* ip6h = (const struct ip6_hdr*)ip_layer;
		uint8_t next_header = ip6h->ip6_nxt;

		// Для упрощения предположим, что нет extension headers
		const uint8_t* transport_layer = ip_layer + sizeof(struct ip6_hdr);
		size_t transport_len = ip_layer_len - sizeof(struct ip6_hdr);

		// Определяем реальный протокол транспортного уровня
		// В реальности нужно обрабатывать extension headers
		uint8_t proto = next_header;

		// TCP проверки для IPv6
		if (proto == IPPROTO_TCP && transport_len >= sizeof(struct tcphdr)) {
			const struct tcphdr* tcph = (const struct tcphdr*)transport_layer;
			uint16_t src_port = ntohs(tcph->th_sport);
			uint16_t dst_port = ntohs(tcph->th_dport);
			uint8_t tcp_flags = tcph->th_flags;

			// dst port 80 and (tcp[tcpflags] & tcp-syn) != 0
			if (dst_port == 80 && (tcp_flags & TH_SYN) != 0) {
				return true;
			}

			// dst port 443 and (tcp[tcpflags] & (tcp-syn|tcp-ack)) == (tcp-syn|tcp-ack)
			if (dst_port == 443 && (tcp_flags & (TH_SYN | TH_ACK)) == (TH_SYN | TH_ACK)) {
				return true;
			}

			// dst port 22 and (tcp[tcpflags] & tcp-syn) != 0
			if (dst_port == 22 && (tcp_flags & TH_SYN) != 0) {
				return true;
			}

			// dst portrange 10000-20000 and not src port 53
			if (dst_port >= 10000 && dst_port <= 20000 && src_port != 53) {
				return true;
			}
		}

		// UDP проверки для IPv6
		else if (proto == IPPROTO_UDP && transport_len >= sizeof(struct udphdr)) {
			const struct udphdr* udph = (const struct udphdr*)transport_layer;
			uint16_t src_port = ntohs(udph->uh_sport);
			uint16_t dst_port = ntohs(udph->uh_dport);
			uint16_t udp_length = ntohs(udph->uh_ulen);

			// dst port 53 and length > 100
			if (dst_port == 53 && udp_length > 100) {
				return true;
			}

			// Для IPv6 нет TTL поля как в IPv4, пропускаем проверку ip[8]
			// src port 67 and dst port 68 and ether[0] & 1 == 0
			if (src_port == 67 && dst_port == 68 && is_unicast_mac(eth->ether_dhost)) {
				return true;
			}

			// dst port 5060 and udp[20:2] != 0x5349
			if (dst_port == 5060) {
				const uint8_t* udp_payload = transport_layer + sizeof(struct udphdr);
				size_t udp_payload_len = transport_len - sizeof(struct udphdr);

				if (udp_payload_len >= 22) {
					uint16_t value = get_u16_safe(udp_payload, udp_payload_len, 20);
					if (value != 0x5349) {
						return true;
					}
				}
			}

			// dst port 1900 - для IPv6 проверка ip[8] и ip[9] не применима
			// Можно пропустить или адаптировать
		}
	}

	return false;
}