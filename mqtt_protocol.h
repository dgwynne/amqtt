/* */

/*
 * Copyright (c) 2021 David Gwynne <david@gwynne.id.au>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#define MQTT_MAX_REMLEN		268435455
#define MQTT_MAX_LEN		0xffff

#define MQTT_T_CONNECT		1
#define MQTT_T_CONNACK		2
#define MQTT_T_PUBLISH		3
#define MQTT_T_PUBACK		4
#define MQTT_T_PUBREC		5
#define MQTT_T_PUBREL		6
#define MQTT_T_PUBCOMP		7
#define MQTT_T_SUBSCRIBE	8
#define MQTT_T_SUBACK		9
#define MQTT_T_UNSUBSCRIBE	10
#define MQTT_T_UNSUBACK		11
#define MQTT_T_PINGREQ		12
#define MQTT_T_PINGRESP		13
#define MQTT_T_DISCONNECT	14

#define MQTT_TYPE(_t)		((_t) << 4)

/*
 * this represents the maximum sized header, not necessarily the
 * actual header on the wire.
 */

struct mqtt_header {
	uint8_t			p;
	uint8_t			remlen[4];
};

struct mqtt_u16 {
	uint8_t			hi;
	uint8_t			lo;
};

struct mqtt_p_connect {
	struct mqtt_u16		len;
	uint8_t			mqtt[4];
	uint8_t			level;
	uint8_t			flags;
#define MQTT_CONNECT_F_CLEAN_SESSION		(1 << 1)
#define MQTT_CONNECT_F_WILL			(1 << 2)
#define MQTT_CONNECT_F_WILL_QOS(_qos)		((_qos) << 3)
#define MQTT_CONNECT_F_WILL_RETAIN		(1 << 5)
#define MQTT_CONNECT_F_PASSWORD			(1 << 6)
#define MQTT_CONNECT_F_USERNAME			(1 << 7)
	struct mqtt_u16		keep_alive;
};

struct mqtt_p_connack {
	uint8_t			flags;
#define MQTT_CONNACK_F_SP			(1 << 0)
	uint8_t			code;
#define MQTT_CONNACK_ACCEPTED			0x00
#define MQTT_CONNACK_PROTO_VERSION		0x01
#define MQTT_CONNACK_IDENTIFIER			0x02
#define MQTT_CONNACK_SERVER_UNAVAILABLE		0x03
#define MQTT_CONNACK_BAD_CREDENTIALS		0x04
#define MQTT_CONNACK_NOT_AUTHORIZED		0x05
};
