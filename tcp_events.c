#define _GNU_SOURCE

#include "tcp_events.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pcap/pcap.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include "constants.h"
#include "init.h"
#include "json_builder.h"
#include "lib.h"
#include "logger.h"
#include "packet_sniffer.h"
#include "resizable_array.h"
#include "string_builders.h"
#include "verbose_mode.h"

#define MUTEX_ERRORCHECK PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP

static pthread_mutex_t connections_count_mutex = MUTEX_ERRORCHECK;
static int connections_count = 0;

/* Private functions */

static TcpConnection *alloc_connection(void) {
        TcpConnection *con;
        if (!(con = (TcpConnection *)my_calloc(sizeof(TcpConnection), 1)))
                goto error;

        // Get & increment connections_count
        mutex_lock(&connections_count_mutex);
        con->id = connections_count;
        connections_count++;
        mutex_unlock(&connections_count_mutex);

        // Has to be done AFTER getting the con->id
        con->directory = create_numbered_dir_in_path(conf_opt_d, con->id);
        return con;
error:
        LOG_FUNC_FAIL;
        return NULL;
}

static TcpEvent *alloc_event(TcpEventType type, int return_value, int err,
                             int id) {
        bool success;
        TcpEvent *ev;
        switch (type) {
                case TCP_EV_SOCKET:
                        ev = (TcpEvent *)my_calloc(sizeof(TcpEvSocket), 1);
                        success = (return_value != 0);
                        break;
                case TCP_EV_BIND:
                        success = (return_value != -1);
                        ev = (TcpEvent *)my_calloc(sizeof(TcpEvBind), 1);
                        break;
                case TCP_EV_CONNECT:
                        success = (return_value != -1);
                        ev = (TcpEvent *)my_calloc(sizeof(TcpEvConnect), 1);
                        break;
                case TCP_EV_SHUTDOWN:
                        success = (return_value != -1);
                        ev = (TcpEvent *)my_calloc(sizeof(TcpEvShutdown), 1);
                        break;
                case TCP_EV_LISTEN:
                        success = (return_value != -1);
                        ev = (TcpEvent *)my_calloc(sizeof(TcpEvListen), 1);
                        break;
                case TCP_EV_SETSOCKOPT:
                        success = (return_value != -1);
                        ev = (TcpEvent *)my_calloc(sizeof(TcpEvSetsockopt), 1);
                        break;
                case TCP_EV_SEND:
                        success = (return_value != -1);
                        ev = (TcpEvent *)my_calloc(sizeof(TcpEvSend), 1);
                        break;
                case TCP_EV_RECV:
                        success = (return_value != -1);
                        ev = (TcpEvent *)my_calloc(sizeof(TcpEvRecv), 1);
                        break;
                case TCP_EV_SENDTO:
                        success = (return_value != -1);
                        ev = (TcpEvent *)my_calloc(sizeof(TcpEvSendto), 1);
                        break;
                case TCP_EV_RECVFROM:
                        success = (return_value != -1);
                        ev = (TcpEvent *)my_calloc(sizeof(TcpEvRecvfrom), 1);
                        break;
                case TCP_EV_SENDMSG:
                        success = (return_value != -1);
                        ev = (TcpEvent *)my_calloc(sizeof(TcpEvSendmsg), 1);
                        break;
                case TCP_EV_RECVMSG:
                        success = (return_value != -1);
                        ev = (TcpEvent *)my_calloc(sizeof(TcpEvRecvmsg), 1);
                        break;
                case TCP_EV_WRITE:
                        success = (return_value != -1);
                        ev = (TcpEvent *)my_calloc(sizeof(TcpEvWrite), 1);
                        break;
                case TCP_EV_READ:
                        success = (return_value != -1);
                        ev = (TcpEvent *)my_calloc(sizeof(TcpEvRead), 1);
                        break;
                case TCP_EV_CLOSE:
                        success = (return_value == 0);
                        ev = (TcpEvent *)my_calloc(sizeof(TcpEvClose), 1);
                        break;
                case TCP_EV_WRITEV:
                        success = (return_value != -1);
                        ev = (TcpEvent *)my_calloc(sizeof(TcpEvWritev), 1);
                        break;
                case TCP_EV_READV:
                        success = (return_value != -1);
                        ev = (TcpEvent *)my_calloc(sizeof(TcpEvReadv), 1);
                        break;
                case TCP_EV_TCP_INFO:
                        success = (return_value != -1);
                        ev = (TcpEvent *)my_calloc(sizeof(TcpEvTcpInfo), 1);
                        break;
        }
        if (!ev) goto error;
        fill_timeval(&(ev->timestamp));
        ev->type = type;
        ev->return_value = return_value;
        ev->success = success;
        ev->error_str = success ? NULL : alloc_error_str(err);
        ev->id = id;
        return ev;
error:
        LOG_FUNC_FAIL;
        return NULL;
}

static void free_addr(TcpAddr *addr) {
        free(addr->ip);
        free(addr->port);
        free(addr->name);
        free(addr->serv);
}

static void free_event(TcpEvent *ev) {
        free(ev->error_str);
        switch (ev->type) {
                case TCP_EV_BIND:
                        free_addr(&((TcpEvBind *)ev)->addr);
                        break;
                case TCP_EV_CONNECT:
                        free_addr(&((TcpEvConnect *)ev)->addr);
                        break;
                case TCP_EV_SETSOCKOPT:
                        free(((TcpEvSetsockopt *)ev)->optname_str);
                        break;
                case TCP_EV_READV:
                        free(((TcpEvReadv *)ev)->iovec.iovec_sizes);
                        break;
                case TCP_EV_WRITEV:
                        free(((TcpEvWritev *)ev)->iovec.iovec_sizes);
                        break;
                default:
                        break;
        }
        free(ev);
}

static void free_events_list(TcpEventNode *head) {
        TcpEventNode *tmp;
        while (head != NULL) {
                free_event(head->data);
                tmp = head;
                head = head->next;
                free(tmp);
        }
}

static void push_event(TcpConnection *con, TcpEvent *ev) {
        TcpEventNode *node = (TcpEventNode *)my_malloc(sizeof(TcpEventNode));
        if (!node) goto error;

        node->data = ev;
        node->next = NULL;
        if (!con->head)
                con->head = node;
        else
                con->tail->next = node;
        con->tail = node;
        con->events_count++;
        return;
error:
        LOG_FUNC_FAIL;
        return;
}

static void fill_addr(TcpAddr *a, const struct sockaddr *addr, socklen_t len) {
        memcpy(&(a->addr_sto), addr, len);
        a->ip = alloc_ip_str(addr);
        a->port = alloc_port_str(addr);
        alloc_name_str(addr, len, &a->name, &a->serv);
}

static void fill_send_flags(TcpSendFlags *s, int flags) {
        s->msg_confirm = (flags & MSG_CONFIRM);
        s->msg_dontroute = (flags & MSG_DONTROUTE);
        s->msg_dontwait = (flags & MSG_DONTWAIT);
        s->msg_eor = (flags & MSG_EOR);
        s->msg_more = (flags & MSG_MORE);
        s->msg_nosignal = (flags & MSG_NOSIGNAL);
        s->msg_oob = (flags & MSG_OOB);
}

static void fill_recv_flags(TcpRecvFlags *s, int flags) {
        s->msg_cmsg_cloexec = (flags & MSG_CMSG_CLOEXEC);
        s->msg_dontwait = (flags & MSG_DONTWAIT);
        s->msg_errqueue = (flags & MSG_ERRQUEUE);
        s->msg_oob = (flags & MSG_OOB);
        s->msg_peek = (flags & MSG_PEEK);
        s->msg_trunc = (flags & MSG_TRUNC);
        s->msg_waitall = (flags & MSG_WAITALL);
}

static socklen_t fill_iovec(TcpIovec *iov1, const struct iovec *iov2,
                            int iovec_count) {
        iov1->iovec_count = iovec_count;
        if (iovec_count <= 0) return 0;

        iov1->iovec_sizes = (size_t *)my_malloc(sizeof(size_t *) * iovec_count);
        if (!iov1->iovec_sizes) goto error;

        socklen_t bytes = 0;
        for (int i = 0; i < iovec_count; i++) {
                if (iov1->iovec_sizes) iov1->iovec_sizes[i] = iov2[i].iov_len;
                bytes += iov2[i].iov_len;
        }
        return bytes;
error:
        LOG_FUNC_FAIL;
        return -1;
}

static socklen_t fill_msghdr(TcpMsghdr *m1, const struct msghdr *m2) {
        memcpy(&m1->addr, m2->msg_name, m2->msg_namelen);
        m1->control_data = (m2->msg_control != NULL);
        return fill_iovec(&m1->iovec, m2->msg_iov, m2->msg_iovlen);
}

static void tcp_dump_json(TcpConnection *con, bool final) {
        if (con->directory == NULL) goto error1;
        LOG_FUNC_D;

        char *json_str, *json_file_str;

        if (!(json_file_str = alloc_json_path_str(con))) goto error_out;
        FILE *fp = fopen(json_file_str, "a");
        free(json_file_str);
        if (!fp) goto error_out;

        TcpEventNode *tmp, *cur = con->head;
        while (cur != NULL) {
                TcpEvent *ev = cur->data;
                if (!(json_str = alloc_tcp_ev_json(ev))) goto error_out;

                if (ev->id == 0) my_fputs("[\n", fp);
                my_fputs(json_str, fp);
                if (final && ev->id + 1 == con->events_count)
                        my_fputs("\n", fp);
                else
                        my_fputs(",\n", fp);

                free(json_str);
                free_event(cur->data);
                tmp = cur;
                cur = cur->next;
                free(tmp);
        }
        con->head = NULL;
        con->tail = NULL;
        con->last_json_dump_evcount = con->events_count;

        if (final) my_fputs("]", fp);
        if (fclose(fp) == EOF) goto error2;
        return;
error2:
        LOG(ERROR, "fclose() failed. %s.", strerror(errno));
        goto error_out;
error1:
        LOG(ERROR, "con->directory is NULL.");
error_out:
        LOG_FUNC_FAIL;
        return;
}

#define MIN_PORT 32768  // cat /proc/sys/net/ipv4/ip_local_port_range
#define MAX_PORT 60999
static int force_bind(int fd, TcpConnection *con, bool IPV6) {
        con->force_bind = true;

        for (int port = MIN_PORT; port <= MAX_PORT; port++) {
                int rc;
                if (IPV6) {
                        struct sockaddr_in6 a;
                        a.sin6_family = AF_INET6;
                        a.sin6_port = htons(port);  // Any port
                        a.sin6_addr = in6addr_any;
                        rc = bind(fd, (struct sockaddr *)&a, sizeof(a));
                } else {
                        struct sockaddr_in a;
                        a.sin_family = AF_INET;
                        a.sin_port = htons(port);
                        a.sin_addr.s_addr = INADDR_ANY;
                        rc = bind(fd, (struct sockaddr *)&a, sizeof(a));
                }
                if (rc == 0) return 0;                 // Sucessfull bind. Stop.
                if (errno != EADDRINUSE) goto error1;  // Unexpected error.
                // Expected error EADDRINUSE. Try next port.
        }
        // Could not bind if we reach this point.
        goto error_out;
error1:
        LOG(ERROR, "bind() failed. %s.", strerror(errno));
        goto error_out;
error_out:
        LOG_FUNC_FAIL;
        return -1;
}

static bool should_dump_tcp_info(const TcpConnection *con) {
        /* Check if time lower bound is set, otherwise assume no lower bound */
        if (conf_opt_u > 0) {
                long cur_time = get_time_micros();
                long time_elasped = cur_time - con->last_info_dump_micros;
                if (time_elasped < conf_opt_u) return false;
        }

        /* Check if bytes lower bound set, otherwise assume no lower bound */
        if (conf_opt_b > 0) {
                long cur_bytes = con->bytes_sent + con->bytes_received;
                long bytes_elapsed = cur_bytes - con->last_info_dump_bytes;
                if (bytes_elapsed < conf_opt_b) return false;
        }

        /* If we reach this point, no lower bound prevents from dumping */
        return true;
}

static bool should_dump_json(const TcpConnection *con) {
        return con->events_count - con->last_json_dump_evcount >= conf_opt_e;
}

/* Public functions */

void free_connection(TcpConnection *con) {
        if (!con) return;  // NULL
        free_events_list(con->head);
        free(con->directory);
        free(con);
}

void tcp_start_capture(int fd, const struct sockaddr *addr_to) {
        TcpConnection *con = ra_get_and_lock_elem(fd);
        if (!con) goto error_out;

        // We force a bind if the socket is not bound. This allows us to know
        // the source port and use a more specific filter for the capture.
        // Before forcing the bind, we unlock the mutex in order to avoid
        // having to use recusive mutexes (since it will trigger a bind()).
        if (!con->bound) {
                con = NULL;
                ra_unlock_elem(fd);
                if (force_bind(fd, con, addr_to->sa_family == AF_INET6))
                        LOG(INFO, "Filter dest IP/PORT only.");
                con = ra_get_and_lock_elem(fd);
                if (!con) goto error_out;
        }

        // Build pcap file path
        char *pcap_file_path = alloc_pcap_path_str(con);
        if (!pcap_file_path) goto error1;

        // Build capture filter
        const struct sockaddr *addr_from =
            (con->bound) ? (const struct sockaddr *)&con->bound_addr : NULL;

        const char *capture_filter = alloc_capture_filter(addr_from, addr_to);
        if (!capture_filter) goto error2;
        con->capture_switch = start_capture(capture_filter, pcap_file_path);

        free(pcap_file_path);
        return;
error2:
        free(pcap_file_path);
error1:
        ra_unlock_elem(fd);
error_out:
        LOG_FUNC_FAIL;
        return;
}

#define TCP_EV_PRELUDE(ev_type_cons, ev_type)                                 \
        TcpConnection *con = ra_get_and_lock_elem(fd);                        \
        if (!con) {                                                           \
                LOG_FUNC_FAIL;                                                \
                return;                                                       \
        }                                                                     \
        const char *ev_name = string_from_tcp_event_type(ev_type_cons);       \
        LOG(INFO, "%s on connection %d.", ev_name, con->id);                  \
        ev_type *ev = (ev_type *)alloc_event(ev_type_cons, return_value, err, \
                                             con->events_count);              \
        if (!ev) {                                                            \
                LOG_FUNC_FAIL;                                                \
                ra_unlock_elem(fd);                                           \
                return;                                                       \
        }

#define TCP_EV_POSTLUDE(ev_type_cons)                                     \
        push_event(con, (TcpEvent *)ev);                                  \
        output_event((TcpEvent *)ev);                                     \
        bool dump_tcp_info =                                              \
            should_dump_tcp_info(con) && ev_type_cons != TCP_EV_TCP_INFO; \
        if (should_dump_json(con)) tcp_dump_json(con, false);             \
        ra_unlock_elem(fd);                                               \
        if (dump_tcp_info) {                                              \
                struct tcp_info _i;                                       \
                int _r = fill_tcpinfo(fd, &_i);                           \
                int _e = errno;                                           \
                tcp_ev_tcp_info(fd, _r, _e, &_i);                         \
        }

const char *string_from_tcp_event_type(TcpEventType type) {
        static const char *strings[] = {
            "socket", "bind", "connect", "shutdown", "listen",  "setsockopt",
            "send",   "recv", "sendto",  "recvfrom", "sendmsg", "recvmsg",
            "write",  "read", "close",   "writev",   "readv",   "tcp_info"};
        assert(sizeof(strings) / sizeof(char *) == TCP_EV_TCP_INFO + 1);
        return strings[type];
}

#define SOCK_TYPE_MASK 0b1111
void tcp_ev_socket(int fd, int domain, int type, int protocol) {
        init_tcpsnitch();
        LOG(INFO, "tcp_ev_socket() with fd %d.", fd);

        /* Check if connection already exits and was not properly closed. */
        if (ra_is_present(fd)) tcp_ev_close(fd, 0, 0, false);

        /* Create new connection */
        TcpConnection *new_con = alloc_connection();
        if (!new_con) goto error;

        /* Create new event */
        TcpEvSocket *ev = (TcpEvSocket *)alloc_event(TCP_EV_SOCKET, fd, 0, 0);
        if (!ev) goto error;

        ev->domain = domain;
        ev->type = type & SOCK_TYPE_MASK;
        ev->protocol = protocol;
        ev->sock_cloexec = type & SOCK_CLOEXEC;
        ev->sock_nonblock = type & SOCK_NONBLOCK;

        push_event(new_con, (TcpEvent *)ev);
        output_event((TcpEvent *)ev);
        if (!ra_put_elem(fd, new_con)) goto error;
        return;
error:
        LOG_FUNC_FAIL;
        return;
}

void tcp_ev_bind(int fd, int return_value, int err, const struct sockaddr *addr,
                 socklen_t len) {
        // Instantiate local vars TcpConnection *con & TcpEvBind *ev
        TCP_EV_PRELUDE(TCP_EV_BIND, TcpEvBind);

        fill_addr(&(ev->addr), addr, len);
        ev->force_bind = con->force_bind;
        if (!return_value) {
                con->bound = true;
                memcpy(&con->bound_addr, &ev->addr.addr_sto,
                       sizeof(struct sockaddr_storage));
        }

        TCP_EV_POSTLUDE(TCP_EV_BIND)
}

void tcp_ev_connect(int fd, int return_value, int err,
                    const struct sockaddr *addr, socklen_t len) {
        // Instantiate local vars TcpConnection *con & TcpEvConnect *ev
        TCP_EV_PRELUDE(TCP_EV_CONNECT, TcpEvConnect);

        fill_addr(&(ev->addr), addr, len);

        TCP_EV_POSTLUDE(TCP_EV_CONNECT)
}

void tcp_ev_shutdown(int fd, int return_value, int err, int how) {
        // Instantiate local vars TcpConnection *con & TcpEvShutdown *ev
        TCP_EV_PRELUDE(TCP_EV_SHUTDOWN, TcpEvShutdown);

        ev->shut_rd = (how == SHUT_RD) || (how == SHUT_RDWR);
        ev->shut_wr = (how == SHUT_WR) || (how == SHUT_RDWR);

        TCP_EV_POSTLUDE(TCP_EV_SHUTDOWN)
}

void tcp_ev_listen(int fd, int return_value, int err, int backlog) {
        // Instantiate local vars TcpConnection *con & TcpEvListen *ev
        TCP_EV_PRELUDE(TCP_EV_LISTEN, TcpEvListen);

        ev->backlog = backlog;

        TCP_EV_POSTLUDE(TCP_EV_LISTEN)
}

void tcp_ev_setsockopt(int fd, int return_value, int err, int level,
                       int optname) {
        // Instantiate local vars TcpConnection *con & TcpEvSetsockopt
        // *ev
        TCP_EV_PRELUDE(TCP_EV_SETSOCKOPT, TcpEvSetsockopt);

        struct protoent *p = getprotobynumber(ev->level);

        ev->level = level;
        ev->level_str = p ? p->p_name : NULL;
        ev->optname = optname;
        ev->optname_str = alloc_sock_optname_str(ev->optname);

        TCP_EV_POSTLUDE(TCP_EV_SETSOCKOPT)
}

void tcp_ev_send(int fd, int return_value, int err, size_t bytes, int flags) {
        // Instantiate local vars TcpConnection *con & TcpEvSend *ev
        TCP_EV_PRELUDE(TCP_EV_SEND, TcpEvSend);

        ev->bytes = bytes;
        con->bytes_sent += bytes;
        fill_send_flags(&(ev->flags), flags);

        TCP_EV_POSTLUDE(TCP_EV_SEND)
}

void tcp_ev_recv(int fd, int return_value, int err, size_t bytes, int flags) {
        // Instantiate local vars TcpConnection *con & TcpEvRecv *ev
        TCP_EV_PRELUDE(TCP_EV_RECV, TcpEvRecv);

        ev->bytes = bytes;
        con->bytes_received += bytes;
        fill_recv_flags(&(ev->flags), flags);

        TCP_EV_POSTLUDE(TCP_EV_RECV)
}

void tcp_ev_sendto(int fd, int return_value, int err, size_t bytes, int flags,
                   const struct sockaddr *addr, socklen_t len) {
        // Instantiate local vars TcpConnection *con & TcpEvSendto *ev
        TCP_EV_PRELUDE(TCP_EV_SENDTO, TcpEvSendto);

        ev->bytes = bytes;
        con->bytes_sent += bytes;
        fill_send_flags(&(ev->flags), flags);
        memcpy(&(ev->addr), addr, len);

        TCP_EV_POSTLUDE(TCP_EV_SENDTO)
}

void tcp_ev_recvfrom(int fd, int return_value, int err, size_t bytes, int flags,
                     const struct sockaddr *addr, socklen_t len) {
        // Instantiate local vars TcpConnection *con & TcpEvRecvfrom *ev
        TCP_EV_PRELUDE(TCP_EV_RECVFROM, TcpEvRecvfrom);

        ev->bytes = bytes;
        con->bytes_received += bytes;
        fill_recv_flags(&(ev->flags), flags);
        memcpy(&(ev->addr), addr, len);

        TCP_EV_POSTLUDE(TCP_EV_RECVFROM)
}

void tcp_ev_sendmsg(int fd, int return_value, int err, const struct msghdr *msg,
                    int flags) {
        // Instantiate local vars TcpConnection *con & TcpEvSendmsg *ev
        TCP_EV_PRELUDE(TCP_EV_SENDMSG, TcpEvSendmsg);

        fill_send_flags(&(ev->flags), flags);
        ev->bytes = fill_msghdr(&ev->msghdr, msg);
        con->bytes_sent += ev->bytes;

        TCP_EV_POSTLUDE(TCP_EV_SENDMSG)
}

void tcp_ev_recvmsg(int fd, int return_value, int err, const struct msghdr *msg,
                    int flags) {
        // Instantiate local vars TcpConnection *con & TcpEvRecvmsg *ev
        TCP_EV_PRELUDE(TCP_EV_RECVMSG, TcpEvRecvmsg);

        fill_recv_flags(&(ev->flags), flags);
        ev->bytes = fill_msghdr(&ev->msghdr, msg);
        con->bytes_received += ev->bytes;

        TCP_EV_POSTLUDE(TCP_EV_RECVMSG);
}

void tcp_ev_write(int fd, int return_value, int err, size_t bytes) {
        // Instantiate local vars TcpConnection *con & TcpEvWrite *ev
        TCP_EV_PRELUDE(TCP_EV_WRITE, TcpEvWrite);

        ev->bytes = bytes;
        con->bytes_sent += bytes;

        TCP_EV_POSTLUDE(TCP_EV_WRITE)
}

void tcp_ev_read(int fd, int return_value, int err, size_t bytes) {
        // Instantiate local vars TcpConnection *con & TcpEvRead *ev
        TCP_EV_PRELUDE(TCP_EV_READ, TcpEvRead);

        ev->bytes = bytes;
        con->bytes_received += bytes;

        TCP_EV_POSTLUDE(TCP_EV_READ)
}

void tcp_ev_close(int fd, int return_value, int err, bool detected) {
        TcpConnection *con = ra_remove_elem(fd);
        if (!con) goto error;

        LOG(INFO, "close on connection %d.", con->id);
        TcpEvClose *ev = (TcpEvClose *)alloc_event(TCP_EV_CLOSE, return_value,
                                                   err, con->events_count);
        if (!ev) goto error;

        ev->detected = detected;
        if (con->capture_switch != NULL)
                stop_capture(con->capture_switch, con->rtt * 2);

        push_event(con, (TcpEvent *)ev);
        output_event((TcpEvent *)ev);
        tcp_dump_json(con, true);

        free_connection(con);
        return;
error:
        LOG_FUNC_FAIL;
        return;
}

void tcp_ev_writev(int fd, int return_value, int err, const struct iovec *iovec,
                   int iovec_count) {
        // Instantiate local vars TcpConnection *con & TcpEvWritev *ev
        TCP_EV_PRELUDE(TCP_EV_WRITEV, TcpEvWritev);

        ev->bytes = fill_iovec(&ev->iovec, iovec, iovec_count);
        con->bytes_sent += ev->bytes;

        TCP_EV_POSTLUDE(TCP_EV_WRITEV)
}

void tcp_ev_readv(int fd, int return_value, int err, const struct iovec *iovec,
                  int iovec_count) {
        // Instantiate local vars TcpConnection *con & TcpEvReadv *ev
        TCP_EV_PRELUDE(TCP_EV_READV, TcpEvReadv);

        ev->bytes = fill_iovec(&ev->iovec, iovec, iovec_count);
        con->bytes_received += ev->bytes;

        TCP_EV_POSTLUDE(TCP_EV_READV)
}

void tcp_ev_tcp_info(int fd, int return_value, int err, struct tcp_info *info) {
        // Instantiate local vars TcpConnection *con & TcpEvTcpInfo
        // *ev
        TCP_EV_PRELUDE(TCP_EV_TCP_INFO, TcpEvTcpInfo);
        LOG_FUNC_D;

        memcpy(&(ev->info), &info, sizeof(struct tcp_info));
        con->last_info_dump_bytes = con->bytes_sent + con->bytes_received;
        con->last_info_dump_micros = get_time_micros();
        con->rtt = info->tcpi_rtt;

        TCP_EV_POSTLUDE(TCP_EV_TCP_INFO);
}

void tcp_close_unclosed_connections(void) {
        for (long i = 0; i < ra_get_size(); i++)
                if (ra_is_present(i)) tcp_ev_close(i, 0, 0, false);
}

void tcp_free(void) {
        ra_free();
        mutex_destroy(&connections_count_mutex);
}

void tcp_reset(void) {
        ra_reset();
        mutex_init(&connections_count_mutex);
        connections_count = 0;
}