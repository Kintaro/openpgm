/* vim:ts=8:sts=8:sw=4:noai:noexpandtab
 *
 * Simple receiver using the PGM transport, based on enonblocksyncrecvmsgv :/
 *
 * Copyright (c) 2006-2007 Miru Limited.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */


#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef CONFIG_HAVE_EPOLL
#	include <sys/epoll.h>
#endif
#include <sys/time.h>
#include <sys/types.h>

#include <glib.h>

#ifdef G_OS_UNIX
#	include <netdb.h>
#	include <arpa/inet.h>
#	include <netinet/in.h>
#	include <sys/socket.h>
#	include <sys/uio.h>
#endif

#include <pgm/pgm.h>
#include <pgm/backtrace.h>
#include <pgm/log.h>
#ifdef CONFIG_WITH_HTTP
#	include <pgm/http.h>
#endif
#ifdef CONFIG_WITH_SNMP
#	include <pgm/snmp.h>
#endif


/* globals */

static int g_port = 0;
static const char* g_network = "";
static const char* g_source = "";
static gboolean g_multicast_loop = FALSE;
static int g_udp_encap_port = 0;

static int g_max_tpdu = 1500;
static int g_sqns = 100;

static pgm_transport_t* g_transport = NULL;
static GThread* g_thread = NULL;
static GMainLoop* g_loop = NULL;
static gboolean g_quit;
static int g_quit_pipe[2];

static void on_signal (int, gpointer);
static gboolean on_startup (gpointer);
static gboolean on_mark (gpointer);

static gpointer receiver_thread (gpointer);
static int on_msgv (pgm_msgv_t*, guint, gpointer);


G_GNUC_NORETURN static void
usage (
	const char*	bin
	)
{
	fprintf (stderr, "Usage: %s [options]\n", bin);
	fprintf (stderr, "  -n <network>    : Multicast group or unicast IP address\n");
	fprintf (stderr, "  -a <ip address> : Source unicast IP address\n");
	fprintf (stderr, "  -s <port>       : IP port\n");
	fprintf (stderr, "  -p <port>       : Encapsulate PGM in UDP on IP port\n");
	fprintf (stderr, "  -l              : Enable multicast loopback and address sharing\n");
#ifdef CONFIG_WITH_HTTP
	fprintf (stderr, "  -t              : Enable HTTP administrative interface\n");
#endif
#ifdef CONFIG_WITH_SNMP
	fprintf (stderr, "  -x              : Enable SNMP interface\n");
#endif
	exit (1);
}

int
main (
	int		argc,
	char*		argv[]
	)
{
#if defined(CONFIG_WITH_HTTP) || defined(CONFIG_WITH_SNMP)
	GError* err = NULL;
#endif
#ifdef CONFIG_WITH_HTTP
	gboolean enable_http = FALSE;
#endif
#ifdef CONFIG_WITH_SNMP
	gboolean enable_snmpx = FALSE;
#endif

	g_message ("pgmrecv");

/* parse program arguments */
	const char* binary_name = strrchr (argv[0], '/');
	int c;
	while ((c = getopt (argc, argv, "a:s:n:p:lh"
#ifdef CONFIG_WITH_HTTP
					"t"
#endif
#ifdef CONFIG_WITH_SNMP
					"x"
#endif
					)) != -1)
	{
		switch (c) {
		case 'n':	g_network = optarg; break;
		case 'a':	g_source = optarg; break;
		case 's':	g_port = atoi (optarg); break;
		case 'p':	g_udp_encap_port = atoi (optarg); break;

		case 'l':	g_multicast_loop = TRUE; break;
#ifdef CONFIG_WITH_HTTP
		case 't':	enable_http = TRUE; break;
#endif
#ifdef CONFIG_WITH_SNMP
		case 'x':	enable_snmpx = TRUE; break;
#endif

		case 'h':
		case '?': usage (binary_name);
		}
	}

	log_init();
	pgm_init();

#ifdef CONFIG_WITH_HTTP
	if (enable_http) {
		if (!pgm_http_init (PGM_HTTP_DEFAULT_SERVER_PORT, &err)) {
			g_error ("Unable to start HTTP interface: %s", err->message);
			g_error_free (err);
			pgm_shutdown ();
			return EXIT_FAILURE;
		}
	}
#endif
#ifdef CONFIG_WITH_SNMP
	if (enable_snmpx) {
		if (!pgm_snmp_init (&err)) {
			g_error ("Unable to start SNMP interface: %s", err->message);
			g_error_free (err);
#ifdef CONFIG_WITH_HTTP
			if (enable_http)
				pgm_http_shutdown ();
#endif
			pgm_shutdown ();
			return EXIT_FAILURE;
		}
	}
#endif

	g_loop = g_main_loop_new (NULL, FALSE);

	g_quit = FALSE;
#ifdef G_OS_UNIX
	pipe (g_quit_pipe);
#else
	_pipe (g_quit_pipe, 4096, _O_BINARY | _O_NOINHERIT);
#endif

/* setup signal handlers */
	signal (SIGSEGV, on_sigsegv);
#ifdef SIGHUP
	signal (SIGHUP,  SIG_IGN);
#endif
	pgm_signal_install (SIGINT,  on_signal, g_loop);
	pgm_signal_install (SIGTERM, on_signal, g_loop);

/* delayed startup */
	g_message ("scheduling startup.");
	g_timeout_add (0, (GSourceFunc)on_startup, NULL);

/* dispatch loop */
	g_message ("entering main event loop ... ");
	g_main_loop_run (g_loop);

	g_message ("event loop terminated, cleaning up.");

/* cleanup */
	g_quit = TRUE;
	const char one = '1';
	write (g_quit_pipe[1], &one, sizeof(one));
	g_thread_join (g_thread);
	close (g_quit_pipe[0]);
	close (g_quit_pipe[1]);

	g_main_loop_unref(g_loop);
	g_loop = NULL;

	if (g_transport) {
		g_message ("destroying transport.");

		pgm_transport_destroy (g_transport, TRUE);
		g_transport = NULL;
	}

#ifdef CONFIG_WITH_HTTP
	if (enable_http)
		pgm_http_shutdown();
#endif
#ifdef CONFIG_WITH_SNMP
	if (enable_snmpx)
		pgm_snmp_shutdown();
#endif

	g_message ("PGM engine shutdown.");
	pgm_shutdown ();
	g_message ("finished.");
	return EXIT_SUCCESS;
}

static
void
on_signal (
	int		signum,
	gpointer	user_data
	)
{
	GMainLoop* loop = (GMainLoop*)user_data;
	g_message ("on_signal (signum:%d user_data:%p)",
		   signum, user_data);
	g_main_loop_quit (loop);
}

static
gboolean
on_startup (
	G_GNUC_UNUSED gpointer data
	)
{
	struct pgm_transport_info_t* res = NULL;
	GError* err = NULL;

	g_message ("startup.");
	g_message ("create transport.");

/* parse network parameter into transport address structure */
	char network[1024];
	sprintf (network, "%s", g_network);
	if (!pgm_if_get_transport_info (network, NULL, &res, &err)) {
		g_error ("parsing network parameter: %s", err->message);
		g_error_free (err);
		g_main_loop_quit(g_loop);
		return FALSE;
	}
/* create global session identifier */
	if (!pgm_gsi_create_from_hostname (&res->ti_gsi, &err)) {
		g_error ("creating GSI: %s", err->message);
		g_error_free (err);
		pgm_if_free_transport_info (res);
		g_main_loop_quit(g_loop);
		return FALSE;
	}
/* source-specific multicast (SSM) */
	if (g_source[0]) {
		((struct sockaddr_in*)&res->ti_recv_addrs[0].gsr_source)->sin_addr.s_addr = inet_addr(g_source);
	}
/* UDP encapsulation */
	if (g_udp_encap_port) {
		res->ti_udp_encap_ucast_port = g_udp_encap_port;
		res->ti_udp_encap_mcast_port = g_udp_encap_port;
	}
	if (g_port)
		res->ti_dport = g_port;
	if (!pgm_transport_create (&g_transport, res, &err)) {
		g_error ("creating transport: %s", err->message);
		g_error_free (err);
		pgm_if_free_transport_info (res);
		g_main_loop_quit(g_loop);
		return FALSE;
	}
	pgm_if_free_transport_info (res);

/* set PGM parameters */
	pgm_transport_set_nonblocking (g_transport, TRUE);
	pgm_transport_set_recv_only (g_transport, TRUE, FALSE);
	pgm_transport_set_max_tpdu (g_transport, g_max_tpdu);
	pgm_transport_set_rxw_sqns (g_transport, g_sqns);
	pgm_transport_set_multicast_loop (g_transport, g_multicast_loop);
	pgm_transport_set_hops (g_transport, 16);
	pgm_transport_set_peer_expiry (g_transport, pgm_secs(300));
	pgm_transport_set_spmr_expiry (g_transport, pgm_msecs(250));
	pgm_transport_set_nak_bo_ivl (g_transport, pgm_msecs(50));
	pgm_transport_set_nak_rpt_ivl (g_transport, pgm_secs(2));
	pgm_transport_set_nak_rdata_ivl (g_transport, pgm_secs(2));
	pgm_transport_set_nak_data_retries (g_transport, 50);
	pgm_transport_set_nak_ncf_retries (g_transport, 50);

/* assign transport to specified address */
	if (!pgm_transport_bind (g_transport, &err)) {
		g_error ("binding transport: %s", err->message);
		g_error_free (err);
		pgm_transport_destroy (g_transport, FALSE);
		g_transport = NULL;
		g_main_loop_quit(g_loop);
		return FALSE;
	}

/* create receiver thread */
	g_thread = g_thread_create_full (receiver_thread,
					 g_transport,
					 0,
					 TRUE,
					 TRUE,
					 G_THREAD_PRIORITY_HIGH,
					 &err);
	if (!g_thread) {
		g_error ("g_thread_create_full failed errno %i: \"%s\"", err->code, err->message);
		g_error_free (err);
		pgm_transport_destroy (g_transport, FALSE);
		g_transport = NULL;
		g_main_loop_quit(g_loop);
		return FALSE;
	}

/* period timer to indicate some form of life */
// TODO: Gnome 2.14: replace with g_timeout_add_seconds()
	g_timeout_add(10 * 1000, (GSourceFunc)on_mark, NULL);

	g_message ("startup complete.");
	return FALSE;
}

/* idle log notification
 */

static gboolean
on_mark (
	G_GNUC_UNUSED gpointer data
	)
{
	g_message ("-- MARK --");
	return TRUE;
}

static gpointer
receiver_thread (
	gpointer	data
	)
{
	pgm_transport_t* transport = (pgm_transport_t*)data;
	pgm_msgv_t msgv[ 20 ];

#ifdef CONFIG_HAVE_EPOLL
	struct epoll_event events[1];	/* wait for maximum 1 event */
	int timeout;
	int efd = epoll_create (IP_MAX_MEMBERSHIPS);
	if (efd < 0) {
		g_error ("epoll_create failed errno %i: \"%s\"", errno, strerror(errno));
		g_main_loop_quit(g_loop);
		return NULL;
	}

	if (pgm_transport_epoll_ctl (g_transport, efd, EPOLL_CTL_ADD, EPOLLIN) < 0)
	{
		g_error ("pgm_epoll_ctl failed errno %i: \"%s\"", errno, strerror(errno));
		g_main_loop_quit(g_loop);
		return NULL;
	}
	struct epoll_event event;
	event.events = EPOLLIN;
	event.data.fd = g_quit_pipe[0];
	if (epoll_ctl (efd, EPOLL_CTL_ADD, g_quit_pipe[0], &event) < 0)
	{
		g_error ("epoll_ctl failed errno %i: \"%s\"", errno, strerror(errno));
		g_main_loop_quit(g_loop);
		return NULL;
	}
#elif defined(CONFIG_HAVE_POLL)
	int timeout;
	int n_fds = 2;
	struct pollfd fds[ 1 + n_fds ];
#else
	int n_fds;
	fd_set readfds;
#endif /* !CONFIG_HAVE_EPOLL */

	do {
		struct timeval tv;
		gsize len;
		GError* err = NULL;
		const PGMIOStatus status = pgm_recvmsgv (transport,
						       msgv,
						       G_N_ELEMENTS(msgv),
						       0,
						       &len,
						       &err);
		switch (status) {
		case PGM_IO_STATUS_NORMAL:
			on_msgv (msgv, len, NULL);
			break;
		case PGM_IO_STATUS_TIMER_PENDING:
			pgm_transport_get_timer_pending (g_transport, &tv);
			g_message ("wait on fd or pending timer %u:%u",
				   (unsigned)tv.tv_sec, (unsigned)tv.tv_usec);
			goto block;
		case PGM_IO_STATUS_RATE_LIMITED:
			pgm_transport_get_rate_remaining (g_transport, &tv);
			g_message ("wait on fd or rate limit timeout %u:%u",
				   (unsigned)tv.tv_sec, (unsigned)tv.tv_usec);
		case PGM_IO_STATUS_WOULD_BLOCK:
block:
#ifdef CONFIG_HAVE_EPOLL
			timeout = PGM_IO_STATUS_WOULD_BLOCK == status ? -1 : ((tv.tv_sec * 1000) + (tv.tv_usec / 1000));
			epoll_wait (efd, events, G_N_ELEMENTS(events), timeout /* ms */);
#elif defined(CONFIG_HAVE_POLL)
			timeout = PGM_IO_STATUS_WOULD_BLOCK == status ? -1 : ((tv.tv_sec * 1000) + (tv.tv_usec / 1000));
			memset (fds, 0, sizeof(fds));
			fds[0].fd = g_quit_pipe[0];
			fds[0].events = POLLIN;
			pgm_transport_poll_info (g_transport, &fds[1], &n_fds, POLLIN);
			poll (fds, 1 + n_fds, timeout /* ms */);
#else /* HAVE_SELECT */
			FD_ZERO(&readfds);
			FD_SET(g_quit_pipe[0], &readfds);
			n_fds = g_quit_pipe[0] + 1;
			pgm_transport_select_info (g_transport, &readfds, NULL, &n_fds);
			select (n_fds, &readfds, NULL, NULL, PGM_IO_STATUS_RATE_LIMITED == status ? &tv : NULL);
#endif /* !CONFIG_HAVE_EPOLL */
			break;

		default:
			if (err) {
				g_warning ("%s", err->message);
				g_error_free (err);
			}
			if (PGM_IO_STATUS_ERROR == status)
				break;
		}
	} while (!g_quit);

#ifdef CONFIG_HAVE_EPOLL
	close (efd);
#endif
	return NULL;
}

static int
on_msgv (
	pgm_msgv_t*	msgv,		/* an array of msgvs */
	guint		len,
	G_GNUC_UNUSED gpointer	user_data
	)
{
        g_message ("(%i bytes)",
                        len);

        guint i = 0;
/* for each apdu */
	do {
                struct pgm_sk_buff_t* pskb = msgv[i].msgv_skb[0];
		gsize apdu_len = 0;
		for (unsigned j = 0; j < msgv[i].msgv_len; j++)
			apdu_len += msgv[i].msgv_skb[j]->len;
/* truncate to first fragment to make GLib printing happy */
		char buf[2000], tsi[PGM_TSISTRLEN];
		const gsize buflen = MIN(sizeof(buf) - 1, pskb->len);
		strncpy (buf, (char*)pskb->data, buflen);
		buf[buflen] = '\0';
		pgm_tsi_print_r (&pskb->tsi, tsi, sizeof(tsi));
		if (msgv[i].msgv_len > 1)
			g_message ("\t%u: \"%s\" ... (%" G_GSIZE_FORMAT " bytes from %s)",
				   i, buf, apdu_len, tsi);
		else
			g_message ("\t%u: \"%s\" (%" G_GSIZE_FORMAT " bytes from %s)",
				   i, buf, apdu_len, tsi);
		i++;
		len -= apdu_len;
        } while (len);

	return 0;
}

/* eof */
