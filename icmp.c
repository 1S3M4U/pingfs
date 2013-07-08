#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/ip_icmp.h>
#include <netinet/icmp6.h>

#include "icmp.h"

#define ICMP_MIN_LENGTH 8

struct icmp_rule {
	int request_type;
	int reply_type;
	int use_checksum;
	int strip_iphdr;
};

static const struct icmp_rule icmpv4 = {
	.request_type = ICMP_ECHO,
	.reply_type = ICMP_ECHOREPLY,
	.use_checksum = 1,
	.strip_iphdr = 1,
};
static const struct icmp_rule icmpv6 = {
	.request_type = ICMP6_ECHO_REQUEST,
	.reply_type = ICMP6_ECHO_REPLY,
	.use_checksum = 0,
	.strip_iphdr = 0,
};

#define GET_RULE(x) (((pkt)->peer.ss_family == AF_INET) ? &icmpv4 : &icmpv6 )

static uint16_t checksum(uint8_t *data, uint32_t len)
{
	uint32_t csum = 0;
	uint32_t i;
	for (i = 0; i < len; i += 2) {
		uint16_t c = data[i] << 8;
		if (i + 1 < len) c |= data[i + 1];
		csum += c;
	}
	csum = (csum >> 16) + (csum & 0xffff);
	csum += (csum >> 16);
	return (uint16_t)(~csum);
}

static uint16_t read16(uint8_t *data)
{
	return (data[0] << 8) | data[1];
}

static void write16(uint8_t *data, uint16_t s)
{
	data[0] = s >> 8;
	data[1] = s & 0xFF;
}

static uint8_t *icmp_encode(struct icmp_packet *pkt, int *len)
{
	struct icmp_rule const *rule = GET_RULE(pkt);
	uint8_t *data;
	*len = ICMP_MIN_LENGTH + pkt->payload_len;
	data = malloc(*len);
	bzero(data, *len);

	if (pkt->type == ICMP_REQUEST) {
		data[0] = rule->request_type;
	} else {
		data[0] = rule->reply_type;
	}

	write16(&data[4], pkt->id);
	write16(&data[6], pkt->seqno);
	if (pkt->payload_len)
		memcpy(&data[8], pkt->payload, pkt->payload_len);

	if (rule->use_checksum) {
		write16(&data[2], checksum(data, *len));
	}

	return data;
}

void icmp_send(int socket, struct icmp_packet *pkt)
{
	int len;
	uint8_t *icmpdata = icmp_encode(pkt, &len);

	len = sendto(socket, icmpdata, len, 0, (struct sockaddr *) &pkt->peer, pkt->peer_len);

	free(icmpdata);
}

int icmp_parse(struct icmp_packet *pkt, uint8_t *data, int len)
{
	struct icmp_rule const *rule = GET_RULE(pkt);
	if (rule->strip_iphdr) {
		int hdrlen;
		if (len == 0) return -3;
		hdrlen = (data[0] & 0x0f) << 2;
		if (len < hdrlen) return -4;
		data += hdrlen;
		len -= hdrlen;
	}
	if (len < ICMP_MIN_LENGTH) return -1;
	if (rule->use_checksum) {
		if (checksum(data, len) != 0) return -2;
	}
	if (rule->request_type == data[0]) {
		pkt->type = ICMP_REQUEST;
	} else if (rule->reply_type == data[0]) {
		pkt->type = ICMP_REPLY;
	} else {
		return -5;
	}
	pkt->id = read16(&data[4]);
	pkt->seqno = read16(&data[6]);
	pkt->payload_len = len - ICMP_MIN_LENGTH;
	if (pkt->payload_len) {
		pkt->payload = malloc(pkt->payload_len);
		memcpy(pkt->payload, &data[8], pkt->payload_len);
	} else {
		pkt->payload = NULL;
	}
	return 0;
}

static void *get_in_addr(struct sockaddr_storage *ss)
{
	if (ss->ss_family == AF_INET) {
		return &(((struct sockaddr_in*)ss)->sin_addr);
	} else {
		return &(((struct sockaddr_in6*)ss)->sin6_addr);
	}
}

static char *icmp_type_str(struct icmp_packet *pkt)
{
	if (pkt->type == ICMP_REPLY) return "Reply";
	if (pkt->type == ICMP_REQUEST) return "Request";
	return "Other";
}

void icmp_dump(struct icmp_packet *pkt)
{
	char ipaddr[64];
	bzero(ipaddr, sizeof(ipaddr));
	inet_ntop(pkt->peer.ss_family, get_in_addr(&pkt->peer), ipaddr, pkt->peer_len);

	printf("%s from %s, id %04X, seqno %04X, payload %d bytes\n",
		icmp_type_str(pkt), ipaddr, pkt->id, pkt->seqno, pkt->payload_len);
}

// v4 socket will return full IP header
int open_raw_v4_socket()
{
	return socket(PF_INET, SOCK_RAW, IPPROTO_ICMP);
}

// v6 socket will just give ICMPv6 data, no IP header
int open_raw_v6_socket()
{
	int sock = socket(PF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
	if (sock >= 0) {
		struct icmp6_filter filter;
		ICMP6_FILTER_SETBLOCKALL(&filter);
		ICMP6_FILTER_SETPASS(ICMP6_ECHO_REPLY, &filter);
		setsockopt(sock, IPPROTO_ICMPV6, ICMP6_FILTER, &filter, sizeof(filter));
	}
	return sock;
}

int main(void)
{
	uint8_t buf[2048];
	struct icmp_packet pkt;
	struct sockaddr_in *sockaddr = (struct sockaddr_in *) &pkt.peer;
	struct sockaddr_in6 *sockaddr6 = (struct sockaddr_in6 *) &pkt.peer;
	int fd = open_raw_v4_socket();
	if (fd<0) {
		perror("rawsock");
		exit(1);
	}

	// send
	sockaddr->sin_family = AF_INET;
	sockaddr->sin_port = 0;
	sockaddr->sin_addr.s_addr = inet_addr("173.194.32.2"); //google.com
	pkt.peer_len = sizeof(struct sockaddr_in);
	pkt.type = ICMP_REQUEST;
	pkt.id = 0xFAFE;
	pkt.seqno = 123;
	pkt.payload = strdup("Foo123");
	pkt.payload_len = 7;

	icmp_send(fd, &pkt);
	free(pkt.payload);

	// recv
	pkt.peer_len = sizeof(pkt.peer);
	int i = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *) &pkt.peer, &pkt.peer_len);
	if (i > 0) {
		int d = icmp_parse(&pkt, buf, i);
		printf("parse res=%d\n", d);
		icmp_dump(&pkt);
		free(pkt.payload);
	}

	fd = open_raw_v6_socket();

	// send v6
	sockaddr6->sin6_family = AF_INET6;
	sockaddr6->sin6_port = 0;
	inet_pton(AF_INET6, "2a00:1450:400f:800::1001", &sockaddr6->sin6_addr); // google.com
	pkt.peer_len = sizeof(struct sockaddr_in6);
	pkt.type = ICMP_REQUEST;
	pkt.id = 0xFAFE;
	pkt.seqno = 123;
	pkt.payload = strdup("Foo123");
	pkt.payload_len = 7;

	icmp_send(fd, &pkt);
	free(pkt.payload);

	// recv v6
	pkt.peer_len = sizeof(pkt.peer);
	i = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *) &pkt.peer, &pkt.peer_len);
	if (i > 0) {
		int d = icmp_parse(&pkt, buf, i);
		printf("parse res=%d\n", d);
		icmp_dump(&pkt);
		free(pkt.payload);
	}
	return 0;
}

