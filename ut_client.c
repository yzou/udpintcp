#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <fcntl.h>
#include <syslog.h>
#include <errno.h>
#include <assert.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>

#include "library.h"

static void print_help(int argc, char *argv[])
{
	printf("Usage:\n");
	printf(" %s udp_addr server_addr [-d] [-r]\n", argv[0]);
}

int main(int argc, char *argv[])
{
	struct ut_comm_context ctx;
	char *s_udp_addr, *s_server_addr;
	struct sockaddr_storage udp_addr, server_addr;
	socklen_t udp_alen = 0, server_alen = 0;
	int opt;
	bool is_front_end = false, is_daemon = false;

	while ((opt = getopt(argc, argv, "rdh")) > 0) {
		switch (opt) {
		case 'd': is_daemon = true; break;
		case 'r': is_front_end = true; break;
		default: print_help(argc, argv); exit(1);
		}
	}
	if (argc - optind < 2) {
		print_help(argc, argv);
		exit(1);
	}

	init_comm_context(&ctx, is_front_end);

	s_udp_addr = argv[optind++];
	s_server_addr = argv[optind++];
	get_sockaddr_inx_pair(s_udp_addr, &udp_addr, &udp_alen);
	get_sockaddr_inx_pair(s_server_addr, &server_addr, &server_alen);

	if (ctx.is_front_end) {
		ctx.udp_peer_addr = udp_addr;
		ctx.udp_peer_alen = udp_alen;
	} else {
		ctx.back_end.udpfd = create_udp_server_fd(&udp_addr, udp_alen);
		if (ctx.back_end.udpfd < 0)
			exit(1);
	}

	if (is_daemon)
		do_daemonize();

	openlog("ut-client", LOG_PID|LOG_CONS|LOG_PERROR|LOG_NDELAY, LOG_USER);

	signal(SIGPIPE, SIG_IGN);

	for (;;) {
		fd_set rset;
		struct timeval timeo;
		int maxfd = 0, nfds;
		struct front_end_conn *ce;

		/* Check the upstream connection and reconnect */
		if (ctx.tcpfd < 0 || time(NULL) - ctx.last_tcp_recv >= TCP_DEAD_TIMEOUT) {
			char s_svr_addr[64] = "";
			int svr_port = 0;

			if (ctx.tcpfd >= 0) {
				syslog(LOG_WARNING, "Close TCP connection due to keepalive failure.\n");
				close(ctx.tcpfd);
			}

			ctx.tcpfd = socket(AF_INET, SOCK_STREAM, 0);
			assert(ctx.tcpfd >= 0);

			if (connect(ctx.tcpfd, (struct sockaddr *)&server_addr, server_alen) < 0) {
				syslog(LOG_WARNING, "Failed to connect '%s': %s. Retrying later.\n",
						s_server_addr, strerror(errno));
				destroy_tcp_connection(&ctx);
				sleep(5);
				continue;
			}

			tcp_connection_established(&ctx);

			sockaddr_to_print(&server_addr, s_svr_addr, &svr_port);
			syslog(LOG_INFO, "Connected to server '%s:%d'.\n", s_svr_addr, svr_port);
			continue;
		}

		FD_ZERO(&rset);

		/* The TCP socket */
		if (ctx.tcpfd >= 0 && ctx.tcp_rx_dlen < UT_TCP_RX_BUFFER_SIZE) {
			FD_SET(ctx.tcpfd, &rset);
			SET_IF_LARGER(maxfd, ctx.tcpfd);
		}

		if (ctx.is_front_end) {
			list_for_each_entry (ce, &ctx.front_end.conn_list, list) {
				FD_SET(ce->sockfd, &rset);
				SET_IF_LARGER(maxfd, ce->sockfd);
			}
		} else {
			FD_SET(ctx.back_end.udpfd, &rset);
			SET_IF_LARGER(maxfd, ctx.back_end.udpfd);
		}

		timeo.tv_sec = 0; timeo.tv_usec = 300 * 1000;

		nfds = select(maxfd + 1, &rset, NULL, NULL, &timeo);
		if (nfds == 0) {
			goto keepalive;
		} else if (nfds < 0) {
			if (errno == EINTR || errno == ERESTART) {
				continue;
			} else {
				syslog(LOG_ERR, "*** select() error: %s.\n", strerror(errno));
				exit(1);
			}
		}

		if (FD_ISSET(ctx.tcpfd, &rset)) {
			if (process_tcp_receive(&ctx) < 0)
				goto keepalive;
		}

		if (ctx.is_front_end) {
			list_for_each_entry (ce, &ctx.front_end.conn_list, list) {
				if (FD_ISSET(ce->sockfd, &rset))
					process_udp_receive(&ctx, ce);
			}
		} else {
			if (FD_ISSET(ctx.back_end.udpfd, &rset))
				process_udp_receive(&ctx, NULL);
		}

keepalive:
		/* Send keep-alive packet */
		if (time(NULL) - ctx.last_tcp_send >= KEEPALIVE_INTERVAL) {
			send_tcp_keepalive(&ctx);
			if (ctx.is_front_end)
				recycle_front_end_conn(&ctx);
		}
	}

	return 0;
}

