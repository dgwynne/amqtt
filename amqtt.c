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

#ifndef min
#define min(_a, _b)	((_a) < (_b) ? (_a) : (_b))
#endif

#ifndef ISSET
#define ISSET(_v, _m)	((_v) & (_m))
#endif

struct mqtt_message {
	uint8_t		*mm_buf;
	size_t		 mm_len;
	size_t		 mm_off;
	void		*mm_cookie;
	int		 mm_id;

	TAILQ_ENTRY(mqtt_message)
			 mm_entry;
};

TAILQ_HEAD(mqtt_messages, mqtt_message);

enum mqtt_state {
	MQTT_S_IDLE,
	MQTT_S_REMLEN,

	MQTT_S_MEMCPY,

	MQTT_S_TOPIC_LEN_HI,
	MQTT_S_TOPIC_LEN_LO,
	MQTT_S_PID_HI,
	MQTT_S_PID_LO,
	MQTT_S_PAYLOAD,
	MQTT_S_PUB_DONE,

	MQTT_S_DONE,

	MQTT_S_DEAD,
};

struct mqtt_conn {
	void		*mc_cookie;
	const struct mqtt_settings
			*mc_settings;
	const char	*mc_errstr;

	uint16_t	 mc_id;

	/* output state */
	struct mqtt_messages
			 mc_messages;
	struct mqtt_messages
			 mc_pending;

	/* input parser state */
	enum mqtt_state	 mc_state;
	enum mqtt_state	 mc_nstate;
	int		 mc_type;
	uint8_t		 mc_flags;

	unsigned int	 mc_remlen;
	unsigned int	 mc_shift;

	uint8_t		*mc_mem;
	unsigned int	 mc_len;
	unsigned int	 mc_off;

	uint8_t		*mc_topic;
	unsigned int	 mc_topic_len;
	int		 mc_pid;
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

static uint16_t
mqtt_u16_rd(const void *buf)
{
	const struct mqtt_u16 *mu16 = buf;

	return ((uint16_t)mu16->hi << 8 | (uint16_t)mu16->lo << 0);
}

static size_t
mqtt_u16(void *buf, uint16_t u16)
{
	struct mqtt_u16 *mu16 = buf;

	mu16->hi = u16 >> 8;
	mu16->lo = u16 >> 0;

	return (sizeof(*mu16));
}

static size_t
mqtt_lenstr(void *buf, uint16_t len, const void *str)
{
	uint8_t *bytes = buf;
	size_t blen;

	blen = mqtt_u16(bytes, len);
	memcpy(bytes + blen, str, len);

	return (blen + len);
}

void *
mqtt_cookie(struct mqtt_conn *mc)
{
	return (mc->mc_cookie);
}

const char *
mqtt_errstr(struct mqtt_conn *mc)
{
	return (mc->mc_errstr);
}

struct mqtt_conn *
mqtt_conn_create(const struct mqtt_settings *ms, void *cookie)
{
	struct mqtt_conn *mc;

	mc = malloc(sizeof(*mc));
	if (mc == NULL)
		return (NULL);

	mc->mc_id = arc4random(); /* random starting point */

	mc->mc_cookie = cookie;
	mc->mc_settings = ms;
	TAILQ_INIT(&mc->mc_messages);
	TAILQ_INIT(&mc->mc_pending);

	mc->mc_state = MQTT_S_IDLE;

	return (mc);
}

void
mqtt_conn_destroy(struct mqtt_conn *mc)
{
	free(mc);
}

static int
mqtt_enqueue(struct mqtt_conn *mc, void *cookie, int id, void *msg, size_t len)
{
	struct mqtt_message *mm;

	mm = malloc(sizeof(*mm));
	if (mm == NULL)
		return (-1);

	mm->mm_buf = msg;
	mm->mm_len = len;
	mm->mm_off = 0;
	mm->mm_cookie = cookie;
	mm->mm_id = id;

	TAILQ_INSERT_TAIL(&mc->mc_messages, mm, mm_entry);

	/* push hard */
	mqtt_output(mc);

	return (0);
}

static int
mqtt_id_isset(struct mqtt_messages *mms, int id)
{
	struct mqtt_message *mm;

	TAILQ_FOREACH(mm, mms, mm_entry) {
		if (mm->mm_id == id)
			return (1);
	}

	return (0);
}

static int
mqtt_id(struct mqtt_conn *mc)
{
	int id;

	for (;;) {
		id = mc->mc_id++;

		if (mqtt_id_isset(&mc->mc_messages, id))
			continue;
		if (mqtt_id_isset(&mc->mc_pending, id))
			continue;

		break;
	}

	return (id);
}

static enum mqtt_state
mqtt_memcpy(struct mqtt_conn *mc, size_t len, enum mqtt_state nstate)
{
	mc->mc_mem = malloc(len);
	if (mc->mc_mem == NULL)
		return (MQTT_S_DEAD);

	mc->mc_len = len;
	mc->mc_off = 0;
	mc->mc_nstate = nstate;

	return (MQTT_S_MEMCPY);
}

static enum mqtt_state
mqtt_strcpy(struct mqtt_conn *mc, size_t len, enum mqtt_state nstate)
{
	mc->mc_mem = malloc(len + 1);
	if (mc->mc_mem == NULL)
		return (MQTT_S_DEAD);

	mc->mc_mem[len] = '\0';
	mc->mc_len = len;
	mc->mc_off = 0;
	mc->mc_nstate = nstate;

	return (MQTT_S_MEMCPY);
}

static enum mqtt_state
mqtt_parse(struct mqtt_conn *mc, uint8_t ch)
{
	enum mqtt_state state = mc->mc_state;
	uint8_t type, flags;

	switch (state) {
	case MQTT_S_IDLE:
		type = (ch >> 4) & 0xf;
		flags = (ch >> 0) & 0xf;

		switch (type) {
		case MQTT_T_CONNECT:
			return (MQTT_S_DEAD);
		case MQTT_T_CONNACK:
			if (flags != 0)
				return (MQTT_S_DEAD);
			/* check if this is first? */
			break;

		case MQTT_T_PUBLISH:
			break;

		case MQTT_T_PUBACK:
			return (MQTT_S_DEAD);
		case MQTT_T_PUBREC:
			return (MQTT_S_DEAD);
		case MQTT_T_PUBREL:
			return (MQTT_S_DEAD);
		case MQTT_T_PUBCOMP:
			return (MQTT_S_DEAD);

		case MQTT_T_SUBSCRIBE:
			return (MQTT_S_DEAD);
		case MQTT_T_SUBACK:
			break;

		case MQTT_T_UNSUBSCRIBE:
			return (MQTT_S_DEAD);
		case MQTT_T_UNSUBACK:
			break;

		case MQTT_T_PINGREQ:
			return (MQTT_S_DEAD);
		case MQTT_T_PINGRESP:
			break;

		case MQTT_T_DISCONNECT:
			return (MQTT_S_DEAD);

		default:
			return (MQTT_S_DEAD);
		}

		mc->mc_type = type;
		mc->mc_flags = flags;
		mc->mc_remlen = 0;
		mc->mc_shift = 0;

		return (MQTT_S_REMLEN);

	case MQTT_S_REMLEN:
		mc->mc_remlen |= (unsigned int)(ch & 0x7f) << mc->mc_shift;

		if (ch & 0x80) {
			mc->mc_shift += 7;
			if (mc->mc_shift > 28)
				return (MQTT_S_DEAD);
			return (state);
		}

		if (mc->mc_type == MQTT_T_PUBLISH) {
			if (mc->mc_remlen < sizeof(struct mqtt_u16))
				return (MQTT_S_DEAD);
			mc->mc_remlen -= sizeof(struct mqtt_u16);

			return (MQTT_S_TOPIC_LEN_HI);
		}

		return (mqtt_memcpy(mc, mc->mc_remlen, MQTT_S_DONE));

	case MQTT_S_MEMCPY:
		/* this should be handled in mqtt_input() */
		abort();

	case MQTT_S_TOPIC_LEN_HI:
		mc->mc_topic_len = (unsigned int)ch << 8;
		return (MQTT_S_TOPIC_LEN_LO);
	case MQTT_S_TOPIC_LEN_LO:
		mc->mc_topic_len |= ch;

		if (ISSET(mc->mc_flags, 0x3 << 1)) {
			if (mc->mc_remlen < sizeof(struct mqtt_u16))
				return (MQTT_S_DEAD);
			mc->mc_remlen -= sizeof(struct mqtt_u16);

			state = MQTT_S_PID_HI;
		} else {
			mc->mc_pid = -1;
			state = MQTT_S_PAYLOAD;
		}

		if (mc->mc_topic_len > mc->mc_remlen)
			return (MQTT_S_DEAD);
		mc->mc_remlen -= mc->mc_topic_len;

		return (mqtt_strcpy(mc, mc->mc_topic_len, state));

	case MQTT_S_PID_HI:
		mc->mc_pid = (unsigned int)ch << 8;
		return (MQTT_S_PID_LO);
	case MQTT_S_PID_LO:
		mc->mc_pid |= (unsigned int)ch;

		mc->mc_topic = mc->mc_mem;
		return (mqtt_strcpy(mc, mc->mc_remlen, MQTT_S_PUB_DONE));

	default:
		abort();
	}
}

static enum mqtt_state
mqtt_connack(struct mqtt_conn *mc, const void *mem, size_t len)
{
	const struct mqtt_p_connack *pc;

	if (len < sizeof(*pc))
		return (MQTT_S_DEAD);

	pc = mem;
	if (pc->code != MQTT_CONNACK_ACCEPTED)
		return (MQTT_S_DEAD);

	(*mc->mc_settings->mqtt_on_connect)(mc);

	return (MQTT_S_IDLE);
}

static struct mqtt_message *
mqtt_get_pending(struct mqtt_conn *mc, int pid)
{
	struct mqtt_message *mm;

	/* don't need SAFE here cos we're not iterating past the removal */
	TAILQ_FOREACH(mm, &mc->mc_pending, mm_entry) {
		if (mm->mm_id == pid) {
			TAILQ_REMOVE(&mc->mc_pending, mm, mm_entry);
			return (mm);
		}
	}

	return (NULL);
}

static enum mqtt_state
mqtt_suback(struct mqtt_conn *mc, const void *mem, size_t len)
{
	struct mqtt_message *mm;
	const struct mqtt_u16 *mu16 = mem;
	const uint8_t *buf;
	void *cookie;
	int pid;

	if (len < sizeof(*mu16))
		return (MQTT_S_DEAD);

	pid = mqtt_u16_rd(mu16);
	mm = mqtt_get_pending(mc, pid);
	if (mm == NULL)
		return (MQTT_S_DEAD);

	cookie = mm->mm_cookie;
	free(mm);

	buf = (const uint8_t *)(mu16 + 1);
	len -= sizeof(*mu16);
	if (len == 0)
		return (MQTT_S_DEAD);

	(*mc->mc_settings->mqtt_on_suback)(mc, cookie, buf, len);

	return (MQTT_S_IDLE);
}

static enum mqtt_state
mqtt_nstate(struct mqtt_conn *mc)
{
	enum mqtt_state state = mc->mc_nstate;

	switch (state) {
	case MQTT_S_PID_HI:
		break;
	case MQTT_S_PAYLOAD:
		mc->mc_topic = mc->mc_mem;
		if (mc->mc_remlen > 0) {
			return (mqtt_strcpy(mc, mc->mc_remlen,
			    MQTT_S_PUB_DONE));
		}

		mc->mc_mem = NULL;
		mc->mc_len = 0;
		/* FALLTHROUGH */

	case MQTT_S_PUB_DONE:
		/* we give the topic and payload to the main app */
		(*mc->mc_settings->mqtt_on_message)(mc,
		    mc->mc_topic, mc->mc_topic_len,
		    mc->mc_mem, mc->mc_len,
		    (mc->mc_flags >> 1) & 0x3);
		state = MQTT_S_IDLE;
		break;

	case MQTT_S_DONE:
		switch (mc->mc_type) {
		case MQTT_T_CONNACK:
			state = mqtt_connack(mc, mc->mc_mem, mc->mc_len);
			break;
		case MQTT_T_SUBACK:
			state = mqtt_suback(mc, mc->mc_mem, mc->mc_len);
			break;
		default:
			abort();
		}
		free(mc->mc_mem);
		break;
	default:
		abort();
	}

	return (state);
}

void
mqtt_input(struct mqtt_conn *mc, const void *ptr, size_t len)
{
	enum mqtt_state state = mc->mc_state;
	const uint8_t *buf = ptr;
	size_t rem;

	do {
		switch (state) {
		case MQTT_S_MEMCPY:
			rem = mc->mc_len - mc->mc_off;
			if (len < rem)
				rem = len;
			memcpy(mc->mc_mem + mc->mc_off, buf, rem);
			mc->mc_off += rem;
			if (mc->mc_off == mc->mc_len)
				state = mqtt_nstate(mc);

			buf += rem;
			len -= rem;
			break;
		default:
			state = mqtt_parse(mc, *buf);
			buf++;
			len--;
			break;
		}

		if (state == MQTT_S_DEAD) {
			(*mc->mc_settings->mqtt_dead)(mc);
			return;
		}

		mc->mc_state = state;
	} while (len > 0);
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
		if (mm->mm_off < mm->mm_len) {
			(*mc->mc_settings->mqtt_want_output)(mc);
			return;
		}

		TAILQ_REMOVE(&mc->mc_messages, mm, mm_entry);
		free(mm->mm_buf);
		if (mm->mm_id == -1)
			free(mm);
		else
			TAILQ_INSERT_TAIL(&mc->mc_pending, mm, mm_entry);

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
	int keep_alive = 30;

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
		if (mcs->username_len > MQTT_MAX_LEN)
			return (-1);

		len += sizeof(struct mqtt_u16) + mcs->username_len;
		flags |= MQTT_CONNECT_F_USERNAME;

		if (mcs->password != NULL) {
			if (mcs->password_len > MQTT_MAX_LEN)
				return (-1);

			len += sizeof(struct mqtt_u16) + mcs->password_len;
			flags |= MQTT_CONNECT_F_PASSWORD;
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
	if (mqtt_enqueue(mc, NULL, -1, msg, hlen + len) == -1) {
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
	if (mqtt_enqueue(mc, NULL, -1, msg, hlen + len) == -1) {
		free(msg);
		return (-1);
	}

	return (0);
}

int
mqtt_subscribe(struct mqtt_conn *mc, void *cookie,
    const char *filter, size_t filter_len, enum mqtt_qos qos)
{
	uint8_t *msg, *buf;
	int pid;
	size_t len = 0;
	size_t hlen;

	len += sizeof(struct mqtt_u16); /* pid */

	if (filter_len > MQTT_MAX_LEN)
		return (-1);

	len += sizeof(struct mqtt_u16) + filter_len;
	len += sizeof(uint8_t); /* requested qos */

	if (len > MQTT_MAX_REMLEN)
		return (-1);

	msg = malloc(sizeof(struct mqtt_header) + len);
	if (msg == NULL)
		return (-1);

	hlen = mqtt_header_set(msg, MQTT_T_SUBSCRIBE, 0x2 /* wat */, len);
	buf = msg + hlen;

	pid = mqtt_id(mc);
	buf += mqtt_u16(buf, pid);

	buf += mqtt_lenstr(buf, filter_len, filter);
	*buf = qos;

	/* try to shove the message onto the transport straight away */
	if (mqtt_enqueue(mc, cookie, pid, msg, hlen + len) == -1) {
		free(msg);
		return (-1);
	}

	return (0);
}
