/*	$OpenBSD: frontend.c,v 1.4 2017/03/20 16:13:27 florian Exp $	*/

/*
 * Copyright (c) 2017 Florian Obser <florian@openbsd.org>
 * Copyright (c) 2005 Claudio Jeker <claudio@openbsd.org>
 * Copyright (c) 2004 Esben Norby <norby@openbsd.org>
 * Copyright (c) 2003, 2004 Henning Brauer <henning@openbsd.org>
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
#include <sys/ioctl.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/syslog.h>
#include <sys/uio.h>

#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#include <net/route.h>

#include <arpa/inet.h>

#include <netinet/in.h>
#include <netinet/if_ether.h>
#include <netinet/ip6.h>
#include <netinet6/ip6_var.h>
#include <netinet/icmp6.h>

#include <errno.h>
#include <event.h>
#include <ifaddrs.h>
#include <imsg.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "log.h"
#include "slaacd.h"
#include "frontend.h"
#include "control.h"

#define	ALLROUTER	"ff02::2"

__dead void	 frontend_shutdown(void);
void		 frontend_sig_handler(int, short, void *);
void		 update_iface(uint32_t, char*);
void		 frontend_startup(void);
void		 route_receive(int, short, void *);
void		 icmp6_receive(int, short, void *);
int		 get_flags(char *);
int		 get_xflags(char *);
void		 get_lladdr(char *, struct ether_addr *, struct sockaddr_in6 *);
void		 send_solicitation(uint32_t);

struct imsgev			*iev_main;
struct imsgev			*iev_engine;
struct event			 ev_route;
struct msghdr			 sndmhdr;
struct iovec			 sndiov[2];
struct nd_router_solicit	 rs;
struct sockaddr_in6		 dst;
int		 icmp6sock, routesock, xflagssock;

struct icmp6_ev {
	struct event		 ev;
	uint8_t			 answer[1500];
	struct msghdr		 rcvmhdr;
	struct iovec		 rcviov[1];
	struct sockaddr_in6	 from;
} icmp6ev;

void
frontend_sig_handler(int sig, short event, void *bula)
{
	/*
	 * Normal signal handler rules don't apply because libevent
	 * decouples for us.
	 */

	switch (sig) {
	case SIGINT:
	case SIGTERM:
		frontend_shutdown();
	default:
		fatalx("unexpected signal");
	}
}

void
frontend(int debug, int verbose, char *sockname)
{
	struct event		 ev_sigint, ev_sigterm;
	struct passwd		*pw;
	struct icmp6_filter	 filt;
	struct in6_pktinfo	*pi;
	struct cmsghdr		*cm;
	size_t			 rcvcmsglen, sndcmsglen;
	int			 hoplimit = 255, on = 1, rtfilter;
	uint8_t			*rcvcmsgbuf, *sndcmsgbuf;

	log_init(debug, LOG_DAEMON);
	log_setverbose(verbose);

	/* Create slaacd control socket outside chroot. */
	if (control_init(sockname) == -1)
		fatalx("control socket setup failed");

	if ((pw = getpwnam(SLAACD_USER)) == NULL)
		fatal("getpwnam");

	if (chroot(pw->pw_dir) == -1)
		fatal("chroot");
	if (chdir("/") == -1)
		fatal("chdir(\"/\")");

	slaacd_process = PROC_FRONTEND;
	setproctitle(log_procnames[slaacd_process]);
	log_procinit(log_procnames[slaacd_process]);

	if ((icmp6sock = socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6)) < 0)
		fatal("ICMPv6 socket");

	if ((routesock = socket(PF_ROUTE, SOCK_RAW, 0)) < 0)
		fatal("route socket");

	if ((xflagssock = socket(AF_INET6, SOCK_DGRAM, 0)) < 0)
		fatal("socket");

	if (setgroups(1, &pw->pw_gid) ||
	    setresgid(pw->pw_gid, pw->pw_gid, pw->pw_gid) ||
	    setresuid(pw->pw_uid, pw->pw_uid, pw->pw_uid))
		fatal("can't drop privileges");

	if (setsockopt(icmp6sock, IPPROTO_IPV6, IPV6_RECVPKTINFO, &on,
	    sizeof(on)) < 0)
		fatal("IPV6_RECVPKTINFO");

	if (setsockopt(icmp6sock, IPPROTO_IPV6, IPV6_RECVHOPLIMIT, &on,
	    sizeof(on)) < 0)
		fatal("IPV6_RECVHOPLIMIT");

	/* only router advertisements */
	ICMP6_FILTER_SETBLOCKALL(&filt);
	ICMP6_FILTER_SETPASS(ND_ROUTER_ADVERT, &filt);
	if (setsockopt(icmp6sock, IPPROTO_ICMPV6, ICMP6_FILTER, &filt,
	    sizeof(filt)) == -1)
		fatal("ICMP6_FILTER");

	rtfilter = ROUTE_FILTER(RTM_IFINFO) | ROUTE_FILTER(RTM_NEWADDR);
	if (setsockopt(routesock, PF_ROUTE, ROUTE_MSGFILTER,
	    &rtfilter, sizeof(rtfilter)) < 0)
		fatal("setsockopt(ROUTE_MSGFILTER)");

	if (pledge("stdio inet recvfd route", NULL) == -1)
		fatal("pledge");

	event_init();

	/* Setup signal handler. */
	signal_set(&ev_sigint, SIGINT, frontend_sig_handler, NULL);
	signal_set(&ev_sigterm, SIGTERM, frontend_sig_handler, NULL);
	signal_add(&ev_sigint, NULL);
	signal_add(&ev_sigterm, NULL);
	signal(SIGPIPE, SIG_IGN);
	signal(SIGHUP, SIG_IGN);

	/* Setup pipe and event handler to the parent process. */
	if ((iev_main = malloc(sizeof(struct imsgev))) == NULL)
		fatal(NULL);
	imsg_init(&iev_main->ibuf, 3);
	iev_main->handler = frontend_dispatch_main;
	iev_main->events = EV_READ;
	event_set(&iev_main->ev, iev_main->ibuf.fd, iev_main->events,
	    iev_main->handler, iev_main);
	event_add(&iev_main->ev, NULL);

	/* Listen on control socket. */
	TAILQ_INIT(&ctl_conns);
	control_listen();

	event_set(&ev_route, routesock, EV_READ | EV_PERSIST, route_receive,
	    NULL);

	event_set(&icmp6ev.ev, icmp6sock, EV_READ | EV_PERSIST, icmp6_receive,
	    NULL);

	rcvcmsglen = CMSG_SPACE(sizeof(struct in6_pktinfo)) +
	    CMSG_SPACE(sizeof(int));
	if((rcvcmsgbuf = malloc(rcvcmsglen)) == NULL)
		fatal("malloc");

	icmp6ev.rcviov[0].iov_base = (caddr_t)icmp6ev.answer;
	icmp6ev.rcviov[0].iov_len = sizeof(icmp6ev.answer);
	icmp6ev.rcvmhdr.msg_name = (caddr_t)&icmp6ev.from;
	icmp6ev.rcvmhdr.msg_namelen = sizeof(icmp6ev.from);
	icmp6ev.rcvmhdr.msg_iov = icmp6ev.rcviov;
	icmp6ev.rcvmhdr.msg_iovlen = 1;
	icmp6ev.rcvmhdr.msg_control = (caddr_t) rcvcmsgbuf;
	icmp6ev.rcvmhdr.msg_controllen = rcvcmsglen;

	sndcmsglen = CMSG_SPACE(sizeof(struct in6_pktinfo)) +
	    CMSG_SPACE(sizeof(int));

	if ((sndcmsgbuf = malloc(sndcmsglen)) == NULL)
		fatal("malloc");

	rs.nd_rs_type = ND_ROUTER_SOLICIT;
	rs.nd_rs_code = 0;
	rs.nd_rs_cksum = 0;
	rs.nd_rs_reserved = 0;

	memset(&dst, 0, sizeof(dst));
	dst.sin6_family = AF_INET6;	
	if (inet_pton(AF_INET6, ALLROUTER, &dst.sin6_addr.s6_addr) != 1)
		fatal("inet_pton");

	sndmhdr.msg_namelen = sizeof(struct sockaddr_in6);
	sndmhdr.msg_iov = sndiov;
	sndmhdr.msg_iovlen = 1;
	sndmhdr.msg_control = (caddr_t)sndcmsgbuf;
	sndmhdr.msg_controllen = sndcmsglen;

	sndmhdr.msg_name = (caddr_t)&dst;
	sndmhdr.msg_iov[0].iov_base = (caddr_t)&rs;
	sndmhdr.msg_iov[0].iov_len = sizeof(rs);

	cm = CMSG_FIRSTHDR(&sndmhdr);

	cm->cmsg_level = IPPROTO_IPV6;
	cm->cmsg_type = IPV6_PKTINFO;
	cm->cmsg_len = CMSG_LEN(sizeof(struct in6_pktinfo));
	pi = (struct in6_pktinfo *)CMSG_DATA(cm);
	memset(&pi->ipi6_addr, 0, sizeof(pi->ipi6_addr));
	pi->ipi6_ifindex = 0;

	cm = CMSG_NXTHDR(&sndmhdr, cm);
	cm->cmsg_level = IPPROTO_IPV6;
	cm->cmsg_type = IPV6_HOPLIMIT;
	cm->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(cm), &hoplimit, sizeof(int));

	event_dispatch();

	frontend_shutdown();
}

__dead void
frontend_shutdown(void)
{
	/* Close pipes. */
	msgbuf_write(&iev_engine->ibuf.w);
	msgbuf_clear(&iev_engine->ibuf.w);
	close(iev_engine->ibuf.fd);
	msgbuf_write(&iev_main->ibuf.w);
	msgbuf_clear(&iev_main->ibuf.w);
	close(iev_main->ibuf.fd);

	free(iev_engine);
	free(iev_main);

	log_info("frontend exiting");
	exit(0);
}

int
frontend_imsg_compose_main(int type, pid_t pid, void *data,
    uint16_t datalen)
{
	return (imsg_compose_event(iev_main, type, 0, pid, -1, data,
	    datalen));
}

int
frontend_imsg_compose_engine(int type, uint32_t peerid, pid_t pid,
    void *data, uint16_t datalen)
{
	return (imsg_compose_event(iev_engine, type, peerid, pid, -1,
	    data, datalen));
}

void
frontend_dispatch_main(int fd, short event, void *bula)
{
	struct imsg		 imsg;
	struct imsgev		*iev = bula;
	struct imsgbuf		*ibuf = &iev->ibuf;
	ssize_t			 n;
	int			 shut = 0;

	DEBUG_IMSG("%s", __func__);
	
	if (event & EV_READ) {
		if ((n = imsg_read(ibuf)) == -1 && errno != EAGAIN)
			fatal("imsg_read error");
		if (n == 0)	/* Connection closed. */
			shut = 1;
		DEBUG_IMSG("%s: EV_READ, n=%ld", __func__, n);
	}
	if (event & EV_WRITE) {
		if ((n = msgbuf_write(&ibuf->w)) == -1 && errno != EAGAIN)
			fatal("msgbuf_write");
		if (n == 0)	/* Connection closed. */
			shut = 1;
		DEBUG_IMSG("%s: EV_WRITE, n=%ld", __func__, n);
	}

	for (;;) {
		if ((n = imsg_get(ibuf, &imsg)) == -1)
			fatal("%s: imsg_get error", __func__);
		if (n == 0)	/* No more messages. */
			break;


		DEBUG_IMSG("%s: %s", __func__, imsg_type_name[imsg.hdr.type]);
		switch (imsg.hdr.type) {
		case IMSG_SOCKET_IPC:
			/*
			 * Setup pipe and event handler to the engine
			 * process.
			 */
			if (iev_engine) {
				log_warnx("%s: received unexpected imsg fd "
				    "to frontend", __func__);
				break;
			}
			if ((fd = imsg.fd) == -1) {
				log_warnx("%s: expected to receive imsg fd to "
				   "frontend but didn't receive any",
				   __func__);
				break;
			}

			iev_engine = malloc(sizeof(struct imsgev));
			if (iev_engine == NULL)
				fatal(NULL);

			imsg_init(&iev_engine->ibuf, fd);
			iev_engine->handler = frontend_dispatch_engine;
			iev_engine->events = EV_READ;

			event_set(&iev_engine->ev, iev_engine->ibuf.fd,
			iev_engine->events, iev_engine->handler, iev_engine);
			event_add(&iev_engine->ev, NULL);

			if (pledge("stdio inet route", NULL) == -1)
				fatal("pledge");

			break;
		case IMSG_STARTUP:
			frontend_startup();
			break;
		case IMSG_CTL_END:
			control_imsg_relay(&imsg);
			break;
		default:
			log_debug("%s: error handling imsg %d", __func__,
			    imsg.hdr.type);
			break;
		}
		imsg_free(&imsg);
	}
	if (!shut)
		imsg_event_add(iev);
	else {
		/* This pipe is dead. Remove its event handler. */
		event_del(&iev->ev);
		event_loopexit(NULL);
	}
}

void
frontend_dispatch_engine(int fd, short event, void *bula)
{
	struct imsgev		*iev = bula;
	struct imsgbuf		*ibuf = &iev->ibuf;
	struct imsg		 imsg;
	ssize_t			 n;
	int			 shut = 0;
	uint32_t		 if_index;

	DEBUG_IMSG("%s", __func__);

	if (event & EV_READ) {
		if ((n = imsg_read(ibuf)) == -1 && errno != EAGAIN)
			fatal("imsg_read error");
		if (n == 0)	/* Connection closed. */
			shut = 1;
		DEBUG_IMSG("%s: EV_READ, n=%ld", __func__, n);
	}
	if (event & EV_WRITE) {
		if ((n = msgbuf_write(&ibuf->w)) == -1 && errno != EAGAIN)
			fatal("msgbuf_write");
		if (n == 0)	/* Connection closed. */
			shut = 1;
		DEBUG_IMSG("%s: EV_WRITE, n=%ld", __func__, n);
	}

	for (;;) {
		if ((n = imsg_get(ibuf, &imsg)) == -1)
			fatal("%s: imsg_get error", __func__);
		if (n == 0)	/* No more messages. */
			break;

		DEBUG_IMSG("%s: %s", __func__, imsg_type_name[imsg.hdr.type]);

		switch (imsg.hdr.type) {
		case IMSG_CTL_END:
		case IMSG_CTL_SHOW_INTERFACE_INFO:
		case IMSG_CTL_SHOW_INTERFACE_INFO_RA:
		case IMSG_CTL_SHOW_INTERFACE_INFO_RA_PREFIX:
		case IMSG_CTL_SHOW_INTERFACE_INFO_RA_RDNS:
		case IMSG_CTL_SHOW_INTERFACE_INFO_RA_DNSSL:
			control_imsg_relay(&imsg);
			break;
		case IMSG_CTL_SEND_SOLICITATION:
			if_index = *((uint32_t *)imsg.data);
			send_solicitation(if_index);
			break;
		default:
			log_debug("%s: error handling imsg %d", __func__,
			    imsg.hdr.type);
			break;
		}
		imsg_free(&imsg);
	}
	if (!shut)
		imsg_event_add(iev);
	else {
		/* This pipe is dead. Remove its event handler. */
		event_del(&iev->ev);
		event_loopexit(NULL);
	}
}

int
get_flags(char *if_name)
{
	struct ifreq		 ifr;

	(void) strlcpy(ifr.ifr_name, if_name, sizeof(ifr.ifr_name));
	if (ioctl(xflagssock, SIOCGIFFLAGS, (caddr_t)&ifr) < 0)
		fatal("SIOCGIFFLAGS");
	return ifr.ifr_flags;
}

int
get_xflags(char *if_name)
{
	struct ifreq		 ifr;

	(void) strlcpy(ifr.ifr_name, if_name, sizeof(ifr.ifr_name));
	if (ioctl(xflagssock, SIOCGIFXFLAGS, (caddr_t)&ifr) < 0)
		fatal("SIOCGIFXFLAGS");
	return ifr.ifr_flags;
}

void
update_iface(uint32_t if_index, char* if_name)
{
	struct imsg_ifinfo	 imsg_ifinfo;
	int			 flags, xflags;

	flags = get_flags(if_name);
	xflags = get_xflags(if_name);

	if (!(xflags & IFXF_AUTOCONF6))
		return;

	memset(&imsg_ifinfo, 0, sizeof(imsg_ifinfo));

	imsg_ifinfo.if_index = if_index;
	imsg_ifinfo.running = (flags & (IFF_UP | IFF_RUNNING)) == (IFF_UP |
	    IFF_RUNNING);
	imsg_ifinfo.autoconfprivacy = !(xflags & IFXF_INET6_NOPRIVACY);
	get_lladdr(if_name, &imsg_ifinfo.hw_address, &imsg_ifinfo.ll_address);

	frontend_imsg_compose_engine(IMSG_UPDATE_IF, 0, 0, &imsg_ifinfo,
	    sizeof(imsg_ifinfo));
}

void
frontend_startup(void)
{
	struct if_nameindex	*ifnidxp, *ifnidx;

	event_add(&ev_route, NULL);
	event_add(&icmp6ev.ev, NULL);

	if ((ifnidxp = if_nameindex()) == NULL)
		fatalx("if_nameindex");

	for(ifnidx = ifnidxp; ifnidx->if_index !=0 && ifnidx->if_name != NULL;
	    ifnidx++)
		update_iface(ifnidx->if_index, ifnidx->if_name);

	if_freenameindex(ifnidxp);
}

void
route_receive(int fd, short events, void *arg)
{
	static uint8_t		 buf[2048];

	struct if_msghdr	*ifm;
	struct rt_msghdr	*rtm;
	ssize_t			 n;
	int			 flags, xflags, if_index;
	char			 ifnamebuf[IFNAMSIZ];
	char			*if_name;

	do {
		n = read(fd, buf, sizeof(buf));
	} while (n == -1 && errno == EINTR);

	if (n < 0) {
		log_debug("read(routesock)");
		return;
	}

	rtm = (struct rt_msghdr *) buf;
	if ((size_t)n < sizeof(rtm->rtm_msglen) || n < rtm->rtm_msglen ||
	    rtm->rtm_version != RTM_VERSION) {
		log_debug("invalid route message");
		return;
	}

	switch (rtm->rtm_type) {
	case RTM_IFINFO:
		ifm = (struct if_msghdr *)rtm;
		if_name = if_indextoname(ifm->ifm_index, ifnamebuf);
		if (if_name == NULL) {
			log_debug("RTM_IFINFO: lost if %d", ifm->ifm_index);
			if_index = ifm->ifm_index;
			frontend_imsg_compose_engine(IMSG_REMOVE_IF, 0, 0,
			    &if_index, sizeof(if_index));
		} else {
			xflags = get_xflags(if_name);
			flags = get_flags(if_name);
			if (!(xflags & IFXF_AUTOCONF6)) {
				log_debug("RTM_IFINFO: %s(%d) no(longer) "
				   "autoconf6", if_name, ifm->ifm_index);
				if_index = ifm->ifm_index;
				frontend_imsg_compose_engine(IMSG_REMOVE_IF, 0,
				    0, &if_index, sizeof(if_index));
			} else
				update_iface(ifm->ifm_index, if_name);
		}
		break;
	case RTM_NEWADDR:
		ifm = (struct if_msghdr *)rtm;
		if_name = if_indextoname(ifm->ifm_index, ifnamebuf);
		log_debug("RTM_NEWADDR: %s[%u]", if_name, ifm->ifm_index);
		update_iface(ifm->ifm_index, if_name);
		break;
	default:
		log_debug("unexpected RTM: %d", rtm->rtm_type);
		break;
	}
}

void
get_lladdr(char *if_name, struct ether_addr *mac, struct sockaddr_in6 *ll)
{
	struct ifaddrs		*ifap, *ifa;
	struct sockaddr_dl	*sdl;
	struct sockaddr_in6	*sin6;

	if (getifaddrs(&ifap) != 0)
		fatal("getifaddrs");

	memset(mac, 0, sizeof(*mac));
	memset(ll, 0, sizeof(*ll));

	for (ifa = ifap; ifa != NULL; ifa = ifa->ifa_next) {
		if (strcmp(if_name, ifa->ifa_name) != 0)
			continue;

		switch(ifa->ifa_addr->sa_family) {
		case AF_LINK:
			sdl = (struct sockaddr_dl *)ifa->ifa_addr;
			if (sdl->sdl_type != IFT_ETHER ||
			    sdl->sdl_alen != ETHER_ADDR_LEN)
				continue;

			memcpy(mac->ether_addr_octet, LLADDR(sdl),
			    ETHER_ADDR_LEN);
			break;
		case AF_INET6:
			sin6 = (struct sockaddr_in6 *)ifa->ifa_addr;
			if (IN6_IS_ADDR_LINKLOCAL(&sin6->sin6_addr)) {
				sin6->sin6_scope_id = ntohs(*(u_int16_t *)
				    &sin6->sin6_addr.s6_addr[2]);
				sin6->sin6_addr.s6_addr[2] =
				    sin6->sin6_addr.s6_addr[3] = 0;
				memcpy(ll, sin6, sizeof(*ll));
			}
			break;
		default:
			break;
		}
	}
	freeifaddrs(ifap);
}

void
icmp6_receive(int fd, short events, void *arg)
{
	struct imsg_ra		 ra;

	struct in6_pktinfo	*pi = NULL;
	struct cmsghdr		*cm;
	ssize_t			 len;
	int			 if_index = 0, *hlimp = NULL;
	char			 ntopbuf[INET6_ADDRSTRLEN], ifnamebuf[IFNAMSIZ];
	uint8_t			*p;

	p = icmp6ev.answer;

	if ((len = recvmsg(fd, &icmp6ev.rcvmhdr, 0)) < 0) {
		log_warn("recvmsg");
		return;
	}

	/* extract optional information via Advanced API */
	for (cm = (struct cmsghdr *)CMSG_FIRSTHDR(&icmp6ev.rcvmhdr); cm;
	    cm = (struct cmsghdr *)CMSG_NXTHDR(&icmp6ev.rcvmhdr, cm)) {
		if (cm->cmsg_level == IPPROTO_IPV6 &&
		    cm->cmsg_type == IPV6_PKTINFO &&
		    cm->cmsg_len == CMSG_LEN(sizeof(struct in6_pktinfo))) {
			pi = (struct in6_pktinfo *)(CMSG_DATA(cm));
			if_index = pi->ipi6_ifindex;
		}
		if (cm->cmsg_level == IPPROTO_IPV6 &&
		    cm->cmsg_type == IPV6_HOPLIMIT &&
		    cm->cmsg_len == CMSG_LEN(sizeof(int)))
			hlimp = (int *)CMSG_DATA(cm);
	}

	if (if_index == 0) {
		log_warnx("failed to get receiving interface");
		return;
	}

	if (hlimp == NULL) {
		log_warnx("failed to get receiving hop limit");
		return;
	}

	if (*hlimp != 255) {
		log_warn("invalid RA with hop limit of %d from %s on %s",
		    *hlimp, inet_ntop(AF_INET6, &icmp6ev.from.sin6_addr,
		    ntopbuf, INET6_ADDRSTRLEN), if_indextoname(if_index,
		    ifnamebuf));
		return;
	}

	if ((size_t)len > sizeof(ra.packet)) {
		log_warn("invalid RA with size %ld from %s on %s",
		    len, inet_ntop(AF_INET6, &icmp6ev.from.sin6_addr,
		    ntopbuf, INET6_ADDRSTRLEN), if_indextoname(if_index,
		    ifnamebuf));
		return;
	}

	ra.if_index = if_index;
	memcpy(&ra.from,  &icmp6ev.from, sizeof(ra.from));
	ra.len = len;
	memcpy(ra.packet, icmp6ev.answer, len);

	frontend_imsg_compose_engine(IMSG_RA, 0, 0, &ra, sizeof(ra));
}

void
send_solicitation(uint32_t if_index)
{
	struct in6_pktinfo		*pi;
	struct cmsghdr			*cm;

	log_debug("%s(%u)", __func__, if_index);

	dst.sin6_scope_id = if_index;

	cm = CMSG_FIRSTHDR(&sndmhdr);
	pi = (struct in6_pktinfo *)CMSG_DATA(cm);
	pi->ipi6_ifindex = if_index;

	if (sendmsg(icmp6sock, &sndmhdr, 0) != sizeof(rs))
		log_warn("sendmsg");
}
