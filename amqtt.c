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

#include <sys/queue.h>

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "mqtt_protocol.h"
#include "amqtt.h"

struct mqtt_message {
	uint8_t		*mm_buf;
	size_t		 mm_len;
	size_t		 mm_off;
	int		 mm_id;

	TAILQ_ENTRY(mqtt_message)
			 mm_entry;
};

TAILQ_HEAD(mqtt_messages, mqtt_message);

struct mqtt_conn {
	void		*mc_cookie;
	const struct mqtt_settings
			*mc_settings;

	struct mqtt_messages
			 mc_messages;
};

static size_t
mqtt_header_set(void *buf, uint8_t type, uint8_t flags, size_t len)
{
	struct mqtt_header *hdr = buf;
	uint8_t *p = hdr->remlen;
	size_t rv = sizeof(hdr->p);

	hdr->p = (type << 4) | flags;

	do {
		uint8_t byte = len & 0x7f;
		len >>= 7;
		if (len)
			byte |= 0x80;

		*p++ = byte;
		rv++;
	} while (len);

	return (rv);
}

static void
mqtt_u16(struct mqtt_u16 *mu16, uint16_t u16)
{
	mu16->hi = u16 >> 8;
	mu16->lo = u16 >> 0;
}

static size_t
mqtt_lenstr(void *buf, uint16_t len, const void *str)
{
	struct mqtt_u16 *mu16 = buf;

	memcpy(mu16 + 1, str, len);

	return (sizeof(*mu16) + len);
}

void *
mqtt_cookie(struct mqtt_conn *mc)
{
	return (mc->mc_cookie);
}

struct mqtt_conn *
mqtt_conn_create(const struct mqtt_settings *ms, void *cookie)
{
	struct mqtt_conn *mc;

	mc = malloc(sizeof(*mc));
	if (mc == NULL)
		return (NULL);

	mc->mc_cookie = cookie;
	mc->mc_settings = ms;
	TAILQ_INIT(&mc->mc_messages);

	return (mc);
}

void
mqtt_conn_destroy(struct mqtt_conn *mc)
{
	free(mc);
}

static int
mqtt_enqueue(struct mqtt_conn *mc, int id, void *msg, size_t len)
{
	struct mqtt_message *mm;

	mm = malloc(sizeof(*mm));
	if (mm == NULL)
		return (-1);

	mm->mm_buf = msg;
	mm->mm_len = len;
	mm->mm_off = 0;
	mm->mm_id = id;

	TAILQ_INSERT_TAIL(&mc->mc_messages, mm, mm_entry);

	/* push hard */
	mqtt_output(mc);

	return (0);
}

void
mqtt_input(struct mqtt_conn *mc, const void *buf, size_t len)
{

}

void
mqtt_output(struct mqtt_conn *mc)
{
	struct mqtt_message *mm = TAILQ_FIRST(&mc->mc_messages);
	ssize_t rv;

	do {
		rv = (*mc->mc_settings->mqtt_output)(mc,
		    mm->mm_buf + mm->mm_off, mm->mm_len - mm->mm_off);
		if (rv == -1)
			return;

		mm->mm_off += rv;
		if (mm->mm_off <= mm->mm_len) {
			(*mc->mc_settings->mqtt_want_output)(mc);
			return;
		}

		TAILQ_REMOVE(&mc->mc_messages, mm, mm_entry);
		free(mm->mm_buf);
		if (mm->mm_id == -1)
			free(mm);

		mm = TAILQ_FIRST(&mc->mc_messages);
	} while (mm != NULL);
}

int
mqtt_connect(struct mqtt_conn *mc, const struct mqtt_conn_settings *mcs)
{
	uint8_t *msg, *buf;
	struct mqtt_p_connect *pc;
	size_t len = sizeof(*pc);
	size_t hlen;
	uint8_t flags = 0;
	uint16_t keep_alive = 30;

	if (mcs->clean_session)
		flags |= MQTT_CONNECT_F_CLEAN_SESSION;
	if (mcs->keep_alive > 0) {
		keep_alive = mcs->keep_alive;
		if (keep_alive > 0xffff)
			return (-1);
	}

	if (mcs->clientid_len > MQTT_MAX_LEN)
		return (-1);
	len += sizeof(struct mqtt_u16) + mcs->clientid_len;

	if (mcs->will_topic != NULL) {
		if (mcs->will_topic_len > MQTT_MAX_LEN)
			return (-1);
		len += sizeof(struct mqtt_u16) + mcs->will_topic_len;

		if (mcs->will_payload_len > MQTT_MAX_LEN)
			return (-1);
		len += sizeof(struct mqtt_u16) + mcs->will_payload_len;

		flags |= MQTT_CONNECT_F_WILL;
		flags |= MQTT_CONNECT_F_WILL_QOS(mcs->will_qos);
		if (mcs->will_retain)
			flags |= MQTT_CONNECT_F_WILL_RETAIN;
	}

	if (mcs->username != NULL) {
		if (mcs->username_len > 0xfff)
			return (-1);
		len += sizeof(struct mqtt_u16) + mcs->username_len;

		flags |= MQTT_CONNECT_F_USERNAME;

		if (mcs->password != NULL) {
			if (mcs->password_len > MQTT_MAX_LEN)
				return (-1);
			len += sizeof(struct mqtt_u16) + mcs->password_len;
		}
	}

	if (len > MQTT_MAX_REMLEN)
		return (-1);

	msg = malloc(sizeof(struct mqtt_header) + len);
	if (msg == NULL)
		return (-1);

	hlen = mqtt_header_set(msg, MQTT_T_CONNECT, 0, len);
	buf = msg + hlen;

	pc = (struct mqtt_p_connect *)buf;
	mqtt_u16(&pc->len, sizeof(pc->mqtt));
	pc->mqtt[0] = 'M';
	pc->mqtt[1] = 'Q';
	pc->mqtt[2] = 'T';
	pc->mqtt[3] = 'T';
	pc->level = 0x4;
	pc->flags = flags;
	mqtt_u16(&pc->keep_alive, keep_alive);

	buf += sizeof(*pc);

	buf += mqtt_lenstr(buf, mcs->clientid_len, mcs->clientid);
	if (mcs->will_topic != NULL) {
		buf += mqtt_lenstr(buf,
		    mcs->will_topic_len, mcs->will_topic);
		buf += mqtt_lenstr(buf,
		    mcs->will_payload_len, mcs->will_payload);
	}
	if (mcs->username != NULL) {
		buf += mqtt_lenstr(buf,
		    mcs->username_len, mcs->username);
		if (mcs->password != NULL) {
			buf += mqtt_lenstr(buf,
			    mcs->password_len, mcs->password);
		}
	}

	/* try to shove the message onto the transport straight away */
	if (mqtt_enqueue(mc, -1, msg, hlen + len) == -1) {
		free(msg);
		return (-1);
	}

	return (0);
}

void
mqtt_disconnect(struct mqtt_conn *mc)
{

}

int
mqtt_publish(struct mqtt_conn *mc,
    const char *topic, size_t topic_len,
    const char *payload, size_t payload_len,
    enum mqtt_qos qos, unsigned int retain)
{
	uint8_t *msg, *buf;
	size_t len = 0;
	size_t hlen;
	uint8_t flags = 0;

	if (retain)
		flags |= (1 << 0);
	flags |= qos << 1;

	if (topic_len > MQTT_MAX_LEN)
		return (-1);
	len += sizeof(struct mqtt_u16) + topic_len;

	if (qos != MQTT_QOS0) {
		//len += sizeof(struct mqtt_u16);
		return (-1); /* XXX */
	}

	len += payload_len;
	if (len > MQTT_MAX_REMLEN)
		return (-1);

	msg = malloc(sizeof(struct mqtt_header) + len);
	if (msg == NULL)
		return (-1);

	hlen = mqtt_header_set(msg, MQTT_T_PUBLISH, flags, len);
	buf = msg + hlen;

	buf += mqtt_lenstr(buf, topic_len, topic);
	memcpy(buf, payload, payload_len);

	/* try to shove the message onto the transport straight away */
	if (mqtt_enqueue(mc, -1, msg, hlen + len) == -1) {
		free(msg);
		return (-1);
	}

	return (0);
}
