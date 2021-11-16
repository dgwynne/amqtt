
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

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <netdb.h>
#include <errno.h>
#include <err.h>
#include <ctype.h>

#include <event.h>
#include "amqtt.h"

static int		test_connect(int, const char *, const char *);
static int		setnbio(int);

struct test {
	struct mqtt_conn	*mc;
	struct event		 ev_rd;
	struct event		 ev_wr;
	struct event		 ev_tmo;

	const char		*will_topic;
	size_t			 will_topic_len;

	const char		*tele_topic;
	size_t			 tele_topic_len;

	const char		*device;

	int			  argc;
	char			**argv;
};

/* wrappers */

static int	test_mqtt_connect(struct test *, const char *, int);

static void	test_mqtt_rd(int, short, void *);
static void	test_mqtt_wr(int, short, void *);
static void	test_mqtt_tmo(int, short, void *);

/* callbacks */

static void	test_mqtt_want_output(struct mqtt_conn *);
static ssize_t	test_mqtt_output(struct mqtt_conn *, const void *, size_t);
static void	test_mqtt_want_timeout(struct mqtt_conn *,
		    const struct timespec *);

static void	test_mqtt_on_connect(struct mqtt_conn *);
static void	test_mqtt_on_message(struct mqtt_conn *,
		    char *, size_t, char *, size_t,
		    enum mqtt_qos);
static void	test_mqtt_on_suback(struct mqtt_conn *, void *,
		    const uint8_t *, size_t);
static void	test_mqtt_dead(struct mqtt_conn *);

static const struct mqtt_settings test_mqtt_settings = {
	.mqtt_want_output = test_mqtt_want_output,
	.mqtt_output = test_mqtt_output,
	.mqtt_want_timeout = test_mqtt_want_timeout,

	.mqtt_on_connect = test_mqtt_on_connect,
	.mqtt_on_message = test_mqtt_on_message,
	.mqtt_on_suback = test_mqtt_on_suback,
	.mqtt_dead = test_mqtt_dead,
};

__dead static void
usage(void)
{
	extern char *__progname;

	fprintf(stderr, "usage: %s [-46l] [-p port] -d deviceid -h host"
	    " topic...\n", __progname);

	exit(1);
}

int
main(int argc, char *argv[])
{
	struct test *test;
	const char *device = NULL;
	const char *host = NULL;
	const char *port = "1883";
	int lwt = 0;
	int family = AF_UNSPEC;
	int ch;
	int fd;

	while ((ch = getopt(argc, argv, "46d:h:lp:")) != -1) {
		switch (ch) {
		case '4':
			family = AF_INET;
			break;
		case '6':
			family = AF_INET6;
			break;
		case 'd':
			device = optarg;
			break;
		case 'h':
			host = optarg;
			break;
		case 'l':
			lwt = 1;
			break;
		case 'p':
			port = optarg;
			break;
		default:
			usage();
		}
	}

	argc -= optind;
	argv += optind;

	if (argc < 1)
		usage();

	if (host == NULL) {
		warnx("host unspecified");
		usage();
	}
	if (device == NULL) {
		warnx("device unspecified");
		usage();
	}

	fd = test_connect(family, host, port);
	/* test_connect will exit itself */

	test = malloc(sizeof(*test));
	if (test == NULL)
		err(1, NULL);

	memset(test, 0, sizeof(*test));
	if (lwt) {
		char *will_topic;
		int rv;

		rv = asprintf(&will_topic, "tele/%s/LWT", device);
		if (rv == -1)
			errx(1, "will topic");

		test->will_topic = will_topic;
		test->will_topic_len = rv;
	}

	test->device = device;
	test->argc = argc;
	test->argv = argv;

	if (setnbio(fd) == -1)
		err(1, "set non-blocking");

	test->mc = mqtt_conn_create(&test_mqtt_settings, test);
	if (test->mc == NULL)
		err(1, "create mqtt connection");

	event_init();

	event_set(&test->ev_rd, fd, EV_READ|EV_PERSIST,
	    test_mqtt_rd, test);
	event_set(&test->ev_wr, fd, EV_WRITE,
	    test_mqtt_wr, test);
	evtimer_set(&test->ev_tmo, test_mqtt_tmo, test);

	if (test_mqtt_connect(test, device, lwt) == -1)
		errx(1, "mqtt connect failed");

	event_add(&test->ev_rd, NULL);

	event_dispatch();

	return (0);
}

static int
test_mqtt_connect(struct test *test, const char *clientid, int lwt)
{
	struct mqtt_conn_settings mcs = {
		.clean_session = 1,
		.keep_alive = 3,

		.clientid = clientid,
		.clientid_len = strlen(clientid),
	};

	if (test->will_topic != NULL) {
		static const char offline[] = "Offline";

		mcs.will_topic = test->will_topic;
		mcs.will_topic_len = test->will_topic_len;
		mcs.will_payload = offline;
		mcs.will_payload_len = sizeof(offline) - 1;
		mcs.will_retain = 1;
	}

	return (mqtt_connect(test->mc, &mcs));
};

static int
test_connect(int family, const char *host, const char *port)
{
	struct addrinfo hints, *res, *res0;
	int error, serrno;
	int fd;
	const char *cause = NULL;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = family;
	hints.ai_socktype = SOCK_STREAM;

	error = getaddrinfo(host, port, &hints, &res0);
	if (error) {
		errx(1, "host %s port %s: %s", host, port,
		    gai_strerror(error));
	}

	fd = -1;
	for (res = res0; res != NULL; res = res->ai_next) {
		fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
		if (fd == -1) {
			serrno = errno;
			cause = "socket";
			continue;
		}

		if (connect(fd, res->ai_addr, res->ai_addrlen) == -1) {
			serrno = errno;
			cause = "connect";
			close(fd);
			fd = -1;
			continue;
		}

		break;  /* okay we got one */
	}

	if (fd == -1)
		errc(1, serrno, "host %s port %s %s", host, port, cause);

	freeaddrinfo(res0);

	return (fd);
}

static int
setnbio(int fd)
{
	int nbio = 1;

	return (ioctl(fd, FIONBIO, &nbio));
}

void
test_mqtt_rd(int fd, short events, void *arg)
{
	struct test *test = arg;
	struct mqtt_conn *mc = test->mc;
	static char buf[128 << 10];
	ssize_t rv;

	rv = read(fd, buf, sizeof(buf));
	switch (rv) {
	case -1:
		switch (errno) {
		case EAGAIN:
		case EINTR:
			return;
		default:
			break;
		}
		err(1, "%s", __func__);
		/* NOTREACHED */
	case 0:
		mqtt_disconnect(mc);
		mqtt_conn_destroy(mc);
		errx(1, "disconnected");
		/* NOTREACHED */
	default:
		break;

	}

	mqtt_input(mc, buf, rv);
}

void
test_mqtt_wr(int fd, short events, void *arg)
{
	struct test *test = arg;
	struct mqtt_conn *mc = test->mc;

	mqtt_output(mc);
}

static void
test_mqtt_want_output(struct mqtt_conn *mc)
{
	struct test *test = mqtt_cookie(mc);

	event_add(&test->ev_wr, NULL);
}

static void
test_mqtt_tmo(int nil, short events, void *arg)
{
	struct test *test = arg;
	struct mqtt_conn *mc = test->mc;

	mqtt_timeout(mc);
}

static void
test_mqtt_want_timeout(struct mqtt_conn *mc, const struct timespec *ts)
{
	struct test *test = mqtt_cookie(mc);
	struct timeval tv;

	TIMESPEC_TO_TIMEVAL(&tv, ts);

	evtimer_add(&test->ev_tmo, &tv);
}

static ssize_t
test_mqtt_output(struct mqtt_conn *mc, const void *buf, size_t len)
{
	struct test *test = mqtt_cookie(mc);
	int fd = EVENT_FD(&test->ev_wr);
	ssize_t rv;

	rv = write(fd, buf, len);
	if (rv == -1) {
		switch (errno) {
		case EAGAIN:
		case EINTR:
			return (0);
		default:
			break;
		}

		err(1, "%s", __func__);
		/* XXX reconnect */
	}

	return (rv);
}

static void
test_mqtt_on_connect(struct mqtt_conn *mc)
{
	struct test *test = mqtt_cookie(mc);
	static const char online[] = "Online";
	int i;

	if (test->will_topic != NULL) {
		if (mqtt_publish(mc, test->will_topic, test->will_topic_len,
		    online, sizeof(online) - 1,
		    MQTT_QOS0, 1) == -1)
			errx(1, "mqtt_publish %s %s", test->will_topic, online);
	}

	for (i = 0; i < test->argc; i++) {
		const char *arg = test->argv[i];
		if (mqtt_subscribe(mc, NULL,
		    arg, strlen(arg), MQTT_QOS0) == -1)
			errx(1, "mqtt_subscribe %s", arg);
	}
}

static void
test_mqtt_on_message(struct mqtt_conn *mc,
    char *topic, size_t topic_len, char *payload, size_t payload_len,
    enum mqtt_qos qos)
{
	printf("%s %s\n", topic, payload);

	free(topic);
	free(payload);
}

static void
test_mqtt_on_suback(struct mqtt_conn *mc, void *cookie,
    const uint8_t *rcodes, size_t nrcodes)
{

}

static void
test_mqtt_dead(struct mqtt_conn *mc)
{
	err(1, "%s", __func__);
}
