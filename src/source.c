#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <strings.h>
#include <signal.h>
#include <string.h>
#include <fcntl.h>
#include <math.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/param.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>
#include <time.h>
#include <syslog.h>
#include <sys/time.h>
#include <netdb.h>
#include <pthread.h>

#include "common.h"
#include "debug.h"
#include "fg_pcap.h"
#include "fg_socket.h"
#include "fg_time.h"
#include "log.h"
#include "acl.h"
#include "daemon.h"

#ifdef HAVE_FLOAT_H
#include <float.h>
#endif

void remove_flow(unsigned int i);

#ifdef __LINUX__
int get_tcp_info(struct _flow *flow, struct tcp_info *info);
#endif

void init_flow(struct _flow* flow, int is_source);
void uninit_flow(struct _flow *flow);

static int name2socket(struct _flow *flow, char *server_name, unsigned port, struct sockaddr **saptr,
		socklen_t *lenp, char do_connect,
		const int read_buffer_size_req, int *read_buffer_size,
		const int send_buffer_size_req, int *send_buffer_size)
{
	int fd, n;
	struct addrinfo hints, *res, *ressave;
	struct sockaddr_in *tempv4;
	struct sockaddr_in6 *tempv6;
	char service[7];

	bzero(&hints, sizeof(struct addrinfo));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	snprintf(service, sizeof(service), "%u", port);

	if ((n = getaddrinfo(server_name, service, &hints, &res)) != 0) {
		flow_error(flow, "getaddrinfo() failed: %s",
				gai_strerror(n));
		return -1;
	}
	ressave = res;

	do {
		int rc;

		fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
		if (fd < 0)
			continue;

		if (send_buffer_size)
			*send_buffer_size = set_window_size_directed(fd, send_buffer_size_req, SO_SNDBUF);
		if (read_buffer_size)
		*read_buffer_size = set_window_size_directed(fd, read_buffer_size_req, SO_RCVBUF);

		if (!do_connect)
			break;

		rc = connect(fd, res->ai_addr, res->ai_addrlen);
		if (rc == 0) {
			if (res->ai_family == PF_INET) {
				tempv4 = (struct sockaddr_in *) res->ai_addr;
				strncpy(server_name, inet_ntoa(tempv4->sin_addr), 256);
				server_name[255] = 0;
			}
			else if (res->ai_family == PF_INET6){
				tempv6 = (struct sockaddr_in6 *) res->ai_addr;
				inet_ntop(AF_INET6, &tempv6->sin6_addr, server_name, 256);
			}
			break;
		}

		error(ERR_WARNING, "Failed to connect to \"%s:%d\": %s",
				server_name, port, strerror(errno));
		close(fd);
	} while ((res = res->ai_next) != NULL);

	if (res == NULL) {
		flow_error(flow, "Could not establish connection to "
				"\"%s:%d\": %s", server_name, port, strerror(errno));
		freeaddrinfo(ressave);
		return -1;
	}

	if (saptr && lenp) {
		*saptr = malloc(res->ai_addrlen);
		if (*saptr == NULL) {
			error(ERR_FATAL, "malloc(): failed: %s",
					strerror(errno));
		}
		memcpy(*saptr, res->ai_addr, res->ai_addrlen);
		*lenp = res->ai_addrlen;
	}

	freeaddrinfo(ressave);

	return fd;
}

int add_flow_source(struct _request_add_flow_source *request)
{
#ifdef __LINUX__
	socklen_t opt_len = 0;
#endif
	struct _flow *flow;

	if (num_flows >= MAX_FLOWS) {
		logging_log(LOG_WARNING, "Can not accept another flow, already handling MAX_FLOW flows.");
		request_error(&request->r, "Can not accept another flow, already handling MAX_FLOW flows.");
		return -1;
	}

	flow = &flows[num_flows++];
	init_flow(flow, 1);

	flow->settings = request->settings;
	flow->source_settings = request->source_settings;

	flow->write_block = calloc(1, flow->settings.write_block_size);
	flow->read_block = calloc(1, flow->settings.read_block_size);
	if (flow->write_block == NULL || flow->read_block == NULL) {
		logging_log(LOG_ALERT, "could not allocate memory for read/write blocks");
		request_error(&request->r, "could not allocate memory for read/write blocks");
		uninit_flow(flow);
		num_flows--;
		return -1;
	}
	if (flow->settings.byte_counting) {
		int byte_idx;
		for (byte_idx = 0; byte_idx < flow->settings.write_block_size; byte_idx++)
			*(flow->write_block + byte_idx) = (unsigned char)(byte_idx & 0xff);
	}

	flow->fd_reply = name2socket(flow, flow->source_settings.destination_host_reply,
				flow->source_settings.destination_port_reply, NULL, NULL, 1, 0, NULL, 0, NULL);
	if (flow->fd_reply == -1) {
		logging_log(LOG_ALERT, "Could not connect reply socket: %s", flow->error);
		request_error(&request->r, "Could not connect reply socket: %s", flow->error);
		uninit_flow(flow);
		num_flows--;
		return -1;
	}
	flow->state = GRIND_WAIT_CONNECT;
	flow->fd = name2socket(flow, flow->source_settings.destination_host,
			flow->source_settings.destination_port,
			&flow->addr, &flow->addr_len, 0,
			flow->settings.requested_read_buffer_size, &request->real_read_buffer_size,
			flow->settings.requested_send_buffer_size, &request->real_send_buffer_size);
	if (flow->fd == -1) {
		logging_log(LOG_ALERT, "Could not create data socket: %s", flow->error);
		request_error(&request->r, "Could not create data socket: %s", flow->error);
		uninit_flow(flow);
		num_flows--;
		return -1;
	}

	set_non_blocking(flow->fd_reply);

	if (set_flow_tcp_options(flow) == -1) {
		request->r.error = flow->error;
		flow->error = NULL;
		uninit_flow(flow);
		num_flows--;
		return -1;
	}

#ifdef __LINUX__
	opt_len = sizeof(request->cc_alg);
	if (getsockopt(flow->fd, IPPROTO_TCP, TCP_CONG_MODULE,
				request->cc_alg, &opt_len) == -1) {
		request_error(&request->r, "failed to determine actual congestion control algorithm: %s",
			strerror(errno));
		uninit_flow(flow);
		num_flows--;
		return -1;
	}
#endif

	if (!flow->source_settings.late_connect) {
		DEBUG_MSG(4, "(early) connecting test socket");
		connect(flow->fd, flow->addr, flow->addr_len);
		flow->connect_called = 1;
		flow->mtu = get_mtu(flow->fd);
		flow->mss = get_mss(flow->fd);
	}

	request->flow_id = flow->id;

	return 0;
}