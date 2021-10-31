
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

struct mqtt_conn;
struct timeval;

enum mqtt_qos {
	MQTT_QOS0,
	MQTT_QOS1,
	MQTT_QOS2,
};

struct mqtt_settings {
	unsigned int	  mqtt_max_topic;
	unsigned int	  mqtt_max_payload;

	void		(*mqtt_want_output)(struct mqtt_conn *);
	ssize_t		(*mqtt_output)(struct mqtt_conn *,
			      const void *, size_t);
	void		(*mqtt_want_timeout)(struct mqtt_conn *,
			      const struct timeval *);

	void		(*mqtt_on_connect)(struct mqtt_conn *);
	void		(*mqtt_on_message)(struct mqtt_conn *,
			      char *, size_t, char *, size_t,
			      enum mqtt_qos, unsigned int);
	void		(*mqtt_on_suback)(struct mqtt_conn *, int,
			      uint8_t *, size_t);
	void		(*mqtt_on_unsuback)(struct mqtt_conn *, int);
			  
};

struct mqtt_conn_settings {
	unsigned int	 clean_session;
	unsigned int	 keep_alive;

	const char	*clientid;
	size_t		 clientid_len;
	const char	*username;
	size_t		 username_len;
	const char	*password;
	size_t		 password_len;

	const char	*will_topic;
	size_t		 will_topic_len;
	const char	*will_payload;
	size_t		 will_payload_len;
	enum mqtt_qos	 will_qos;
	unsigned int	 will_retain;
};

struct mqtt_conn	*mqtt_conn_create(const struct mqtt_settings *,
			     void *);
int			 mqtt_connect(struct mqtt_conn *,
			     const struct mqtt_conn_settings *);
void			*mqtt_cookie(struct mqtt_conn *);
void			 mqtt_input(struct mqtt_conn *, const void *, size_t);
void			 mqtt_output(struct mqtt_conn *);
void			 mqtt_disconnect(struct mqtt_conn *);
void			 mqtt_conn_destroy(struct mqtt_conn *);

struct mqtt_topic {
	const char	*filter;
	size_t		 len;
	enum mqtt_qos	 qos;
};

int			mqtt_publish(struct mqtt_conn *,
			    const char *, size_t, const char *, size_t,
			    enum mqtt_qos, unsigned int);

int			mqtt_subscribe(struct mqtt_conn *,
			    const char *, size_t, enum mqtt_qos);
int			mqtt_subscribev(struct mqtt_conn *,
			    const struct mqtt_topic *, int);
int			mqtt_unsubscribe(struct mqtt_conn *,
			    const char *, size_t, enum mqtt_qos);
int			mqtt_unsubscribev(struct mqtt_conn *,
			    const struct mqtt_topic *, int);
int			mqtt_ping(struct mqtt_conn *);
