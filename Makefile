#
# $FreeBSD$
#
# Makefile for GuitarHero to MIDI converter
#

VERSION=1.0.0

.if exists(%%PREFIX%%)
PREFIX=		%%PREFIX%%
.else
PREFIX=		/usr/local
.endif

BINDIR=		${PREFIX}/sbin
MANDIR=		${PREFIX}/man/man
LIBDIR?=	${PREFIX}/lib
MKLINT=		no
NOGCCERROR=
NO_PROFILE=
MAN=		# no manual pages at the moment
PROG=		jack_ghero
CFLAGS+=	-I${PREFIX}/include -Wall

.if defined(HAVE_DEBUG)
CFLAGS+=	-DHAVE_DEBUG
CFLAGS+=	-g
.endif

LDFLAGS+=	-L${LIBDIR} -lpthread -ljack -lusbhid

SRCS=		jack_ghero.c

.include <bsd.prog.mk>
