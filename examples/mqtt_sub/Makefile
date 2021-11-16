AMQTT=		${.CURDIR}/../..

.PATH:		${AMQTT}
CFLAGS+=	-I${AMQTT}

PROG=		mqtt_sub
SRCS=		mqtt_sub.c
SRCS+=		amqtt.c
MAN=

LDADD=		-levent
DPADD=		${LIBEVENT}

WARNINGS=	Yes
DEBUG=		-g

.include <bsd.prog.mk>
