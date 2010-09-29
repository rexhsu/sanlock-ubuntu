#include <inttypes.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <syslog.h>
#include <pthread.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>

#include "sm.h"
#include "sm_msg.h"
#include "disk_paxos.h"
#include "sm_options.h"
#include "token_manager.h"
#include "lockfile.h"
#include "log.h"
#include "diskio.h"
#include "sm_client.h"

struct client {
	int fd;
	int pid;
	void *workfn;
	void *deadfn;
	struct token *tokens[MAX_LEASES];
	pthread_t lease_threads[MAX_LEASE_ARGS];
};

#define CLIENT_NALLOC 32 /* TODO: differently */
static int client_maxi;
static int client_size = 0;
static struct client *client = NULL;
static struct pollfd *pollfd = NULL;

/* priorities are LOG_* from syslog.h */
int log_logfile_priority = LOG_ERR;
int log_syslog_priority = LOG_ERR;
int log_stderr_priority = LOG_ERR;

/* sync_manager <action>'s */
#define ACT_INIT	1
#define ACT_DAEMON	2
#define ACT_COMMAND	3
#define ACT_RELEASE	4
#define ACT_SHUTDOWN	5
#define ACT_STATUS	6
#define ACT_LOG_DUMP	7
#define ACT_SET_HOST_ID	8

char command[COMMAND_MAX];
int cluster_mode;
int cmd_argc;
char **cmd_argv;
int external_shutdown;
static int token_id_counter = 1;

struct cmd_acquire_args {
	int ci;
	int token_count;
	int token_ids[MAX_LEASE_ARGS];
	struct sm_header h_recv;
};

static void client_alloc(void)
{
	int i;

	if (!client) {
		client = malloc(CLIENT_NALLOC * sizeof(struct client));
		pollfd = malloc(CLIENT_NALLOC * sizeof(struct pollfd));
	} else {
		client = realloc(client, (client_size + CLIENT_NALLOC) *
					 sizeof(struct client));
		pollfd = realloc(pollfd, (client_size + CLIENT_NALLOC) *
					 sizeof(struct pollfd));
		if (!pollfd)
			log_error(NULL, "can't alloc for pollfd");
	}
	if (!client || !pollfd)
		log_error(NULL, "can't alloc for client array");

	for (i = client_size; i < client_size + CLIENT_NALLOC; i++) {
		memset(&client[i], 0, sizeof(struct client));
		client[i].fd = -1;
		pollfd[i].fd = -1;
		pollfd[i].revents = 0;
	}
	client_size += CLIENT_NALLOC;
}

static void client_dead(int ci)
{
	close(client[ci].fd);
	memset(&client[ci], 0, sizeof(struct client));
	client[ci].fd = -1;
	pollfd[ci].fd = -1;
}

static int client_add(int fd, void (*workfn)(int ci), void (*deadfn)(int ci))
{
	int i;

	if (!client)
		client_alloc();
 again:
	for (i = 0; i < client_size; i++) {
		if (client[i].fd == -1) {
			client[i].workfn = workfn;
			if (deadfn)
				client[i].deadfn = deadfn;
			client[i].fd = fd;
			pollfd[i].fd = fd;
			pollfd[i].events = POLLIN;
			if (i > client_maxi)
				client_maxi = i;
			return i;
		}
	}

	client_alloc();
	goto again;
}

static int find_client_pid(int pid)
{
	int i;

	for (i = 0; i < client_size; i++) {
		if (client[i].pid == pid)
			return i;
	}
	return -1;
}

static int recv_pid(int fd, int *pid)
{
	struct msghdr msg;
	struct cmsghdr *cmsg;
	struct iovec iov;
	struct ucred cred;
	char tmp[CMSG_SPACE(sizeof(struct ucred))];
	char ch;
	int n;

	memset(&msg, 0, sizeof(msg));

	iov.iov_base = &ch;
	iov.iov_len = 1;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = tmp;
	msg.msg_controllen = sizeof(tmp);

	n = recvmsg(fd, &msg, 0);
	if (n != 1)
		return -1;

	cmsg = CMSG_FIRSTHDR(&msg);

	if (cmsg->cmsg_level == SOL_SOCKET &&
	    cmsg->cmsg_type  == SCM_CREDENTIALS) {
		memcpy(&cred, CMSG_DATA(cmsg), sizeof(struct ucred));
		*pid = cred.pid;
		return 0;
	}

	return -1;
}

static void pid_dead(int ci)
{
	struct token **tokens;
	int i;

	log_debug(NULL, "pid_dead ci %d pid %d", ci, client[ci].pid);

	if (client[ci].pid)
		kill(SIGTERM, client[ci].pid);

	/* TODO: cmd_acquire_thread may still be waiting for these
	   leases to be acquired... */

	tokens = client[ci].tokens;

	for (i = 0; i < MAX_LEASES; i++) {
		if (tokens[i] && tokens[i]->acquire_result == 1)
			release_lease(tokens[i]);
	}

	client[ci].pid = 0;
}

static void kill_pids(void)
{
	int ci;

	/* TODO: try killscript first if one is provided */

	for (ci = 0; ci < client_maxi; ci++) {
		if (client[ci].pid)
			kill(SIGTERM, client[ci].pid);
	}

	/* TODO: go back to poll loop in an attempt to clean up some pids
	   from killscript or SIGTERM before calling here again again to
	   use SIGKILL */

	sleep(2);

	for (ci = 0; ci < client_maxi; ci++) {
		if (client[ci].pid)
			kill(SIGTERM, client[ci].pid);
	}
}

static int all_pids_dead(void)
{
	int ci;

	for (ci = 0; ci < client_maxi; ci++) {
		if (client[ci].pid)
			return 0;
	}
	return 1;
}

static int our_host_id_renewed(void)
{
	/* TODO: check if the thread renewing our host_id is still working */
	return 1;
}

static int main_loop(void)
{
	int poll_timeout = to.unstable_poll_ms;
	void (*workfn) (int ci);
	void (*deadfn) (int ci);
	int i, rv, killing_pids = 0;

	while (1) {
		rv = poll(pollfd, client_maxi + 1, poll_timeout);
		if (rv == -1 && errno == EINTR)
			continue;
		if (rv < 0) {
			/* not sure */
		}
		for (i = 0; i <= client_maxi; i++) {
			if (client[i].fd < 0)
				continue;
			if (pollfd[i].revents & POLLIN) {
				workfn = client[i].workfn;
				workfn(i);
			}
			if (pollfd[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
				deadfn = client[i].deadfn;
				if (deadfn)
					deadfn(i);
				client_dead(i);
			}
		}

		if (killing_pids && all_pids_dead())
			break;

		if (external_shutdown || !our_host_id_renewed()) {
			kill_pids();
			killing_pids = 1;
		}
	}

	return 0;
}

static void *cmd_acquire_thread(void *args_in)
{
	struct cmd_acquire_args *args = args_in;
	struct token **tokens;
	struct sm_header h;
	int i, fd, ci, token_count;

	ci = args->ci;
	token_count = args->token_count;
	tokens = client[ci].tokens;
	fd = client[ci].fd;

	memcpy(&h, &args->h_recv, sizeof(struct sm_header));

	for (i = 0; i < token_count; i++)
		pthread_join(client[ci].lease_threads[i], NULL);

	log_debug(NULL, "cmd_acquire_thread joined %d", token_count);

	for (i = 0; i < token_count; i++) {
		if (tokens[i]->acquire_result < 0)
			goto fail;
	}

	h.length = sizeof(h) + (sizeof(int) * token_count);
	h.data = token_count;
	send(fd, &h, sizeof(struct sm_header), MSG_NOSIGNAL);

	free(args);
	return NULL;

 fail:
	for (i = 0; i < token_count; i++) {
		if (tokens[i]->acquire_result == 1)
			release_lease(tokens[i]);
	}

	/* TODO: close disks, free token->disks, free tokens */

	h.length = sizeof(h);
	h.data = 0;
	send(fd, &h, sizeof(struct sm_header), MSG_NOSIGNAL);

	free(args);
	return NULL;
}

/* 1. create a lease_thread to acquire the lease for each token
 * 2. create cmd_acquire_thread to wait for the results of each
 * lease_thread and return the result */

static void cmd_acquire(int ci, struct sm_header *h_recv)
{
	pthread_t wait_thread;
	pthread_attr_t attr;
	struct sm_header h;
	struct token **tokens, *token;
	struct sync_disk *disks;
	int token_ids[MAX_LEASE_ARGS];
	int token_count, added_count;
	int rv, fd, i, disks_len, num_disks;
	struct cmd_acquire_args *args;
	
	token_count = h_recv->data;
	tokens = client[ci].tokens;
	fd = client[ci].fd;

	memset(token_ids, 0, sizeof(token_ids));
	added_count = 0;

	if (token_count > MAX_LEASE_ARGS) {
		log_error(NULL, "client asked for %d leases maximum is %d",
			  token_count, MAX_LEASE_ARGS);
		rv = -1;
		goto fail;
	}

	for (i = 0; i < token_count; i++) {
		disks = NULL;

		token = malloc(sizeof(struct token));
		if (!token) {
			rv = -ENOMEM;
			goto fail;
		}
		memset(token, 0, sizeof(struct token));
		client[ci].tokens[i] = token;

		rv = recv(fd, token, sizeof(struct token), MSG_WAITALL);
		if (rv != sizeof(struct token)) {
			log_error(NULL, "cmd_acquire recv %d %d", rv, errno);
			rv = -1;
			goto fail;
		}
		log_debug(NULL, "cmd_acquire recv t %d %s", rv,
			  token->resource_name);

		num_disks = token->num_disks;

		disks = malloc(num_disks * sizeof(struct sync_disk));
		if (!disks) {
			rv = -ENOMEM;
			goto fail;
		}

		disks_len = num_disks * sizeof(struct sync_disk);
		memset(disks, 0, disks_len);

		rv = recv(fd, disks, disks_len, MSG_WAITALL);
		if (rv != disks_len) {
			log_error(NULL, "cmd_acquire recv %d %d", rv, errno);
			rv = -1;
			goto fail;
		}
		log_debug(NULL, "cmd_acquire recv d %d", rv);
		token->disks = disks;

		token->idx = i;
		token->token_id = token_id_counter++;
		token_ids[i] = token->token_id;

		pthread_attr_init(&attr);
		rv = pthread_create(&(client[ci].lease_threads[i]), &attr, lease_thread, token);
		if (rv < 0)
			goto fail;

		added_count++;
	}

	log_debug(NULL, "cmd_acquire added %d", added_count);

	/* lease_thread created for each token, new thread will wait for
	   the results of the threads and send back a reply */

	args = malloc(sizeof(struct cmd_acquire_args));
	if (!args)
		goto fail;

	memset(args, 0, sizeof(struct cmd_acquire_args));
	args->ci = ci;
	args->token_count = added_count;
	memcpy(args->token_ids, token_ids, sizeof(int) * added_count);
	memcpy(&args->h_recv, h_recv, sizeof(struct sm_header));

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	rv = pthread_create(&wait_thread, &attr, cmd_acquire_thread, args);
	pthread_attr_destroy(&attr);
	if (rv < 0) {
		log_error(NULL, "could not start monitor lease for request");
		free(args);
		goto fail;
	}

	return;

 fail:
	if (disks)
		free(disks);

	for (i = 0; i < added_count; i++)
		pthread_join(client[ci].lease_threads[i], NULL);

	for (i = 0; i < added_count; i++) {
		if (client[ci].tokens[i]) {
			if (client[ci].tokens[i]->acquire_result == 1)
				release_lease(tokens[i]);
			free(client[ci].tokens[i]);
		}
	}

	memcpy(&h, h_recv, sizeof(struct sm_header));
	h.length = sizeof(h);
	h.data = -1;
	send(fd, &h, sizeof(struct sm_header), MSG_NOSIGNAL);
}

static void cmd_release(int fd, struct sm_header *h_recv)
{
	struct token **tokens;
	struct sm_header h;
	char resource_name[NAME_ID_SIZE];
	int results[MAX_LEASE_ARGS];
	int lease_count = h_recv->data;
	int stopped_count;
	int rv, i, j, found;
	int pid, ci_release;

	memset(results, 0, sizeof(results));
	stopped_count = 0;

	rv = recv_pid(fd, &pid);
	if (rv < 0) {
		log_error(NULL, "cmd_release fd %d recv_pid %d", fd, rv);
		return;
	}

	ci_release = find_client_pid(pid);
	if (ci_release < 0) {
		log_error(NULL, "cmd_release no pid %d", pid);
		return;
	}

	log_debug(NULL, "cmd_release fd %d pid %d ci %d lease_count %d",
		  fd, pid, ci_release, lease_count);

	tokens = client[ci_release].tokens;

	for (i = 0; i < lease_count; i++) {
		rv = recv(fd, resource_name, NAME_ID_SIZE, MSG_WAITALL);
		if (rv != NAME_ID_SIZE) {
			log_error(NULL, "cmd_release recv fd %d %d %d", fd, rv, errno);
			results[i] = -1;
			break;
		}

		found = 0;

		for (j = 0; j < MAX_LEASES; j++) {
			if (!tokens[j])
				continue;
					
			if (memcmp(tokens[j]->resource_name, resource_name, NAME_ID_SIZE))
				continue;

			rv = release_lease(tokens[j]);
			free(tokens[j]->disks);
			free(tokens[j]);
			tokens[j] = NULL;
			found = 1;
			break;
		}

		if (found && rv == 1)
			stopped_count++;

		if (found) {
			results[i] = rv;
		} else {
			log_error(NULL, "cmd_release pid %d no resource %s",
				  pid, resource_name);
			results[i] = -ENOENT;
		}
	}

	log_debug(NULL, "cmd_release stopped %d", stopped_count);

	/*
	 * one result for each resource_name received:
	 * 1 = stopped the lease thread, make status query to check result
	 * < 0 = error, no lease thread stopped for this resource_name
	 * 0 = no attempt made to stop this resource_name's lease thread
	 *
	 * h.data is the number of results that are "1"
	 */

	memcpy(&h, h_recv, sizeof(struct sm_header));
	h.length = sizeof(h) + sizeof(int) * lease_count;
	h.data = stopped_count;

	send(fd, &h, sizeof(struct sm_header), MSG_NOSIGNAL);
	send(fd, &results, sizeof(int) * lease_count, MSG_NOSIGNAL);
}

static void cmd_status(int fd, struct sm_header *h_recv)
{
	send(fd, h_recv, sizeof(struct sm_header), MSG_NOSIGNAL);
}

static void cmd_log_dump(int fd, struct sm_header *h_recv)
{
	struct sm_header h;

	memcpy(&h, h_recv, sizeof(struct sm_header));

	/* can't send header until taking log_mutex to find the length */

	write_log_dump(fd, &h);
}

static void cmd_set_host_id(int fd, struct sm_header *h_recv)
{
	struct sm_header h;
	int rv;

	if (options.our_host_id < 0) {
		options.our_host_id = h_recv->data;
		rv = 0;
		log_debug(NULL, "host ID set to %d", options.our_host_id);
	} else if (options.our_host_id == h_recv->data) {
		rv = 0;
	} else {
		rv = 1;
		log_error(NULL, "client tried to reset host ID");
	}
	memcpy(&h, h_recv, sizeof(struct sm_header));
	h.length = sizeof(h);
	h.data = rv;
	send(fd, &h, sizeof(struct sm_header), MSG_NOSIGNAL);
}

static void process_connection(int ci)
{
	struct sm_header h;
	int rv, auto_close = 1;
	int fd = client[ci].fd;
	int pid;

	rv = recv_header(fd, &h);
	if (rv < 0) {
		return;
	}

	log_debug(NULL, "ci %d fd %d cmd %d", ci, fd, h.cmd);

	switch (h.cmd) {
	case SM_CMD_REGISTER:
		rv = recv_pid(fd, &pid);
		if (rv < 0)
			break;
		log_debug(NULL, "cmd_register ci %d pid %d", ci, pid);
		client[ci].pid = pid;
		client[ci].deadfn = pid_dead;
		auto_close = 0;
		break;
	case SM_CMD_ACQUIRE:
		cmd_acquire(ci, &h);
		auto_close = 0;
		break;
	case SM_CMD_RELEASE:
		cmd_release(fd, &h);
		break;
	case SM_CMD_SHUTDOWN:
		external_shutdown = 1;
		break;
	case SM_CMD_STATUS:
		cmd_status(fd, &h);
		break;
	case SM_CMD_LOG_DUMP:
		cmd_log_dump(fd, &h);
		break;
	case SM_CMD_SET_HOST_ID:
		cmd_set_host_id(fd, &h);
		break;
#if 0
	case SM_CMD_GET_TIMEOUTS:
		/* memcpy(sdata, &to, sizeof(to)); */
		break;
	case SM_CMD_SET_TIMEOUTS:
		/* memcpy(&to, rdata, sizeof(to)); */
		break;
	case SM_CMD_NUM_HOSTS:
		/* just rewrite leader block in leases? */
		break;
#endif
	default:
		log_error(NULL, "cmd %d not supported", h.cmd);
	};

	if (auto_close)
		close(client[ci].fd);
}

static void process_listener(int ci GNUC_UNUSED)
{
	int fd;
	int on = 1;

	fd = accept(client[ci].fd, NULL, NULL);
	if (fd < 0)
		return;

	setsockopt(fd, SOL_SOCKET, SO_PASSCRED, &on, sizeof(on));

	client_add(fd, process_connection, NULL);
}

static int setup_listener(void)
{
	int rv, fd;

	rv = setup_listener_socket(MAIN_SOCKET_NAME,
				   sizeof(MAIN_SOCKET_NAME), &fd);
	if (rv < 0)
		return rv;

	client_add(fd, process_listener, NULL);
	return 0;
}

static void sigterm_handler(int sig GNUC_UNUSED)
{
	external_shutdown = 1;
}

static void sigchld_handler(int sig GNUC_UNUSED)
{
}

static int make_dirs(void)
{
	mode_t old_umask;
	int rv;

	old_umask = umask(0022);
	rv = mkdir(SM_RUN_DIR, 0777);
	if (rv < 0 && errno != EEXIST)
		goto out;

	rv = mkdir(DAEMON_LOCKFILE_DIR, 0777);
	if (rv < 0 && errno != EEXIST)
		goto out;

	rv = mkdir(RESOURCE_LOCKFILE_DIR, 0777);
	if (rv < 0 && errno != EEXIST)
		goto out;

	rv = mkdir(DAEMON_SOCKET_DIR, 0777);
	if (rv < 0 && errno != EEXIST)
		goto out;

	rv = mkdir(DAEMON_WATCHDOG_DIR, 0777);
	if (rv < 0 && errno != EEXIST)
		goto out;

	rv = mkdir(SM_LOG_DIR, 0777);
	if (rv < 0 && errno != EEXIST)
		goto out;

	rv = 0;
 out:
	umask(old_umask);
	return rv;
}

static int do_daemon(void)
{
	struct sigaction act;
	int fd, rv;

	/* TODO: daemonize */

	/*
	 * after creating dirs and setting up logging the daemon can
	 * use log_error/log_debug
	 */

	rv = make_dirs();
	if (rv < 0) {
		log_tool("cannot create logging dirs\n");
		return -1;
	}

	setup_logging();

	memset(&act, 0, sizeof(act));
	act.sa_handler = sigterm_handler;
	rv = sigaction(SIGTERM, &act, NULL);
	if (rv < 0)
		return -rv;

	memset(&act, 0, sizeof(act));
	act.sa_handler = sigchld_handler;
	act.sa_flags = SA_NOCLDSTOP;
	rv = sigaction(SIGCHLD, &act, NULL);
	if (rv < 0)
		return -rv;

	fd = lockfile(NULL, DAEMON_LOCKFILE_DIR, DAEMON_NAME);
	if (fd < 0)
		goto out;

	rv = setup_listener();
	if (rv < 0)
		goto out_lockfile;

	/* TODO: create thread here that acquires and renews our host_id lock */

	/* TODO: wait for host_id lock to be acquired */

	rv = main_loop();

	/* TODO: release host_id lock here */

 out_lockfile:
	unlink_lockfile(fd, DAEMON_LOCKFILE_DIR, DAEMON_NAME);
 out:
	close_logging();
	return rv;
}

static int do_init(int token_count, struct token *token_args[],
		   int init_num_hosts, int init_max_hosts)
{
	struct token *token;
	int num_opened;
	int i, rv = 0;

	for (i = 0; i < token_count; i++) {
		token = token_args[i];

		num_opened = open_disks(token->disks, token->num_disks);
		if (!majority_disks(token, num_opened)) {
			log_tool("cannot open majority of disks");
			rv = -1;
			continue;
		}

		rv = disk_paxos_init(token, init_num_hosts, init_max_hosts);
		if (rv < 0) {
			log_tool("cannot initialize disks");
			rv = -1;
		}
	}

	return rv;
}

static void print_usage(void)
{
	printf("Usage:\n");
	printf("sync_manager <action> [options]\n\n");
	printf("main actions:\n");
	printf("  help			print usage\n");
	printf("  init			initialize a lease disk area\n");
	printf("  daemon		start daemon\n");
	printf("\n");
	printf("client actions:\n");
	printf("  command		ask daemon to acquire leases, then run command\n");
	printf("  release		ask daemon to release leases for a given pid\n");
	printf("  status		print internal daemon state\n");
	printf("  log_dump		print internal daemon debug buffer\n");
	printf("  shutdown		ask daemon to kill pids, release leases and exit\n");
	printf("\n");

	printf("\ninit [options] -h <num_hosts> -l LEASE\n");
	printf("  -h <num_hosts>	max host id that will be able to acquire the lease\n");
	printf("  -H <max_hosts>	max number of hosts the disk area will support\n");
	printf("                        (default %d)\n", DEFAULT_MAX_HOSTS);
	printf("  -m <num>		cluster mode of hosts (default 0)\n");
	printf("  -l LEASE		lease description, see below\n");

	printf("\ndaemon [options]\n");
	printf("  -D			print all logging to stderr\n");
	printf("  -L <level>		write logging at level and up to logfile (-1 none)\n");
	printf("  -S <level>		write logging at level and up to syslog (-1 none)\n");
	printf("  -m <num>		cluster mode of hosts (default 0)\n");
	printf("  -i <num>		local host id\n");
	printf("  -w <num>		enable (1) or disable (0) writing watchdog files\n");
	printf("  -a <num>		io_timeout_seconds (-1 no aio)\n");

	printf("\ncommand -l LEASE -c <path> <args>\n");
	printf("  -l LEASE		lease description, see below\n");
	printf("  -c <path> <args>	run command with args, -c must be final option\n");

	printf("\nrelease -p <pid> -r <resource_name>\n");
	printf("  -p <pid>		process whose lease should be released\n");
	printf("  -r <resource_name>	resource name of a previously acquired lease\n");

	printf("\nstatus\n");

	printf("\nlog_dump\n");

	printf("\nshutdown\n");

	printf("\nLEASE = <resource_name>:<path>:<offset>[:<path>:<offset>...]\n");
	printf("  <resource_name>	name of resource being leased\n");
	printf("  <path>		disk path\n");
	printf("  <offset>		offset on disk\n");
	printf("  [:<path>:<offset>...] other disks in a multi-disk lease\n");
	printf("\n");
}

static int add_resource_arg(char *arg, int *token_count, struct token *token_args[])
{
	struct token *token;
	int rv;

	if (*token_count >= MAX_LEASE_ARGS) {
		log_tool("lease args over max %d", MAX_LEASE_ARGS);
		return -1;
	}

	rv = create_token(0, &token);
	if (rv < 0) {
		log_tool("resource arg create");
		return rv;
	}

	strncpy(token->resource_name, arg, NAME_ID_SIZE);
	token_args[*token_count] = token;
	(*token_count)++;
	return rv;
}

/* arg = <resource_name>:<path>:<offset>[:<path>:<offset>...] */

static int add_token_arg(char *arg, int *token_count, struct token *token_args[])
{
	struct token *token;
	char sub[DISK_PATH_LEN + 1];
	int sub_count;
	int colons;
	int num_disks;
	int rv, i, j, d;
	int len = strlen(arg);

	if (*token_count >= MAX_LEASE_ARGS) {
		log_tool("lease args over max %d", MAX_LEASE_ARGS);
		return -1;
	}

	colons = 0;
	for (i = 0; i < strlen(arg); i++) {
		if (arg[i] == '\\') {
			i++;
			continue;
		}

		if (arg[i] == ':')
			colons++;
	}
	if (!colons || (colons % 2)) {
		log_tool("invalid lease arg");
		return -1;
	}
	num_disks = colons / 2;

	if (num_disks > MAX_DISKS) {
		log_tool("invalid lease arg num_disks %d", num_disks);
		return -1;
	}

	rv = create_token(num_disks, &token);
	if (rv < 0) {
		log_tool("lease arg create num_disks %d", num_disks);
		return rv;
	}

	token_args[*token_count] = token;
	(*token_count)++;

	d = 0;
	sub_count = 0;
	j = 0;
	memset(sub, 0, sizeof(sub));

	for (i = 0; i < len + 1; i++) {
		if (arg[i] == '\\') {
			if (i == (len - 1)) {
				log_tool("Invalid lease string");
				goto fail;
			}

			i++;
			sub[j++] = arg[i];
			continue;
		}
		if (i < len && arg[i] != ':') {
			if (j >= DISK_PATH_LEN) {
				log_tool("lease arg length error");
				goto fail;
			}
			sub[j++] = arg[i];
			continue;
		}

		/* do something with sub when we hit ':' or end of arg,
		   first sub is id, odd sub is path, even sub is offset */

		if (!sub_count) {
			if (strlen(sub) > NAME_ID_SIZE) {
				log_tool("lease arg id length error");
				goto fail;
			}
			strncpy(token->resource_name, sub, NAME_ID_SIZE);
		} else if (sub_count % 2) {
			if (strlen(sub) > DISK_PATH_LEN-1 || strlen(sub) < 1) {
				log_tool("lease arg path length error");
				goto fail;
			}
			strncpy(token->disks[d].path, sub, DISK_PATH_LEN - 1);
		} else {
			rv = sscanf(sub, "%llu", (unsigned long long *)&token->disks[d].offset);
			if (rv != 1) {
				log_tool("lease arg offset error");
				goto fail;
			}
			d++;
		}

		sub_count++;
		j = 0;
		memset(sub, 0, sizeof(sub));
	}

	return 0;

 fail:
	free(token->disks);
	free(token);
	return -1;
}

/* TODO: option to start up as the receiving side of a transfer
   from another host.  Watch the other host's lease renewals until
   the other host writes our hostid is written in the leader block,
   at which point the lease is ours and we start doing the renewals.

   TODO: option to transfer the ownership of all leases to a specified
   hostid, then watch for pid to exit? */

/* TODO: option to set timeouts, e.g. -m name1=num,name2=num,name3=num */

#define RELEASE_VERSION "0.0"

static int read_args(int argc, char *argv[],
		     int *token_count, struct token *token_args[],
		     int *action, int *init_num_hosts, int *init_max_hosts)
{
	char optchar;
	char *optionarg;
	char *p;
	char *arg1 = argv[1];
	int optionarg_used;
	int i, j, rv, len, begin_command = 0;

	if (argc < 2 || !strcmp(arg1, "help") || !strcmp(arg1, "--help") ||
	    !strcmp(arg1, "-h")) {
		print_usage();
		exit(EXIT_SUCCESS);
	}

	if (!strcmp(arg1, "version") || !strcmp(arg1, "--version") ||
	    !strcmp(arg1, "-V")) {
		printf("%s %s (built %s %s)\n",
		       argv[0], RELEASE_VERSION, __DATE__, __TIME__);
		exit(EXIT_SUCCESS);
	}

	if (!strcmp(arg1, "init"))
		*action = ACT_INIT;
	else if (!strcmp(arg1, "daemon"))
		*action = ACT_DAEMON;
	else if (!strcmp(arg1, "command"))
		*action = ACT_COMMAND;
	else if (!strcmp(arg1, "release"))
		*action = ACT_RELEASE;
	else if (!strcmp(arg1, "shutdown"))
		*action = ACT_SHUTDOWN;
	else if (!strcmp(arg1, "status"))
		*action = ACT_STATUS;
	else if (!strcmp(arg1, "log_dump"))
		*action = ACT_LOG_DUMP;
	else if (!strcmp(arg1, "set_host_id"))
		*action = ACT_SET_HOST_ID;
	else {
		log_tool("first arg is unknown action");
		print_usage();
		exit(EXIT_FAILURE);
	}

	for (i = 2; i < argc; ) {
		p = argv[i];

		if ((p[0] != '-') || (strlen(p) != 2)) {
			log_tool("unknown option %s", p);
			log_tool("space required before option value");
			print_usage();
			exit(EXIT_FAILURE);
		}

		optchar = p[1];
		i++;

		optionarg = argv[i];
		optionarg_used = 1;

		switch (optchar) {
		case 'D':
			log_stderr_priority = LOG_DEBUG;
			optionarg_used = 0;
			break;
		case 'L':
			log_logfile_priority = atoi(optionarg);
			break;
		case 'S':
			log_syslog_priority = atoi(optionarg);
			break;
		case 'h':
			*init_num_hosts = atoi(optionarg);
			break;
		case 'H':
			*init_max_hosts = atoi(optionarg);
			break;
		case 'm':
			cluster_mode = atoi(optionarg);
			break;
		case 'i':
			options.our_host_id = atoi(optionarg);
			break;
		case 'a':
			to.io_timeout_seconds = atoi(optionarg);
			break;
		case 'r':
			if ((*action) != ACT_RELEASE)
				return -1;

			rv = add_resource_arg(optionarg, token_count, token_args);
			if (rv < 0)
				return rv;
			break;
		case 'p':
			options.pid = atoi(optionarg);
			break;
		case 'l':
			if ((*action) == ACT_RELEASE)
				return -1;

			rv = add_token_arg(optionarg, token_count, token_args);
			if (rv < 0)
				return rv;
			break;
		case 'w':
			options.opt_watchdog = atoi(optionarg);
			break;
		case 'c':
			begin_command = 1;
			optionarg_used = 0;
			break;
		default:
			log_tool("unknown option: %c", optchar);
			exit(EXIT_FAILURE);
		};

		if (optionarg_used)
			i++;

		if (begin_command)
			break;
	}

	/*
	 * the remaining args are for the command
	 *
	 * sync_manager -r foo -n 2 -d bar:0 -c /bin/cmd -X -Y -Z
	 * argc = 12
	 * loop above breaks with i = 8, argv[8] = "/bin/cmd"
	 *
	 * cmd_argc = 4 = argc (12) - i (8)
	 * cmd_argv[0] = "/bin/cmd"
	 * cmd_argv[1] = "-X"
	 * cmd_argv[2] = "-Y"
	 * cmd_argv[3] = "-Z"
	 * cmd_argv[4] = NULL (required by execv)
	 */

	if (begin_command) {
		cmd_argc = argc - i;

		if (cmd_argc < 1) {
			log_tool("command option (-c) requires an arg");
			return -EINVAL;
		}

		len = (cmd_argc + 1) * sizeof(char *); /* +1 for final NULL */
		cmd_argv = malloc(len);
		if (!cmd_argv)
			return -ENOMEM;
		memset(cmd_argv, 0, len);

		for (j = 0; j < cmd_argc; j++) {
			cmd_argv[j] = strdup(argv[i++]);
			if (!cmd_argv[j])
				return -ENOMEM;
		}

		strncpy(command, cmd_argv[0], COMMAND_MAX - 1);
	}

	if ((*action == ACT_DAEMON) && (options.our_host_id < 0) && (*token_count > 0)) {
		log_tool("local host id required is you wish to acquire initial leases");
		return -EINVAL;
	}

	if ((*action == ACT_SET_HOST_ID) && (options.our_host_id < 0)) {
		log_tool("local host id parameter not set");
		return -EINVAL;
	}

	log_debug(NULL, "io_timeout_seconds %d", to.io_timeout_seconds);
	return 0;
}

static void exec_command(void)
{
	if (!command[0]) {
		while (1)
			sleep(10);
	}

	execv(command, cmd_argv);
	perror("execv failed");
}

int main(int argc, char *argv[])
{
	struct token *token_args[MAX_LEASE_ARGS];
	int token_count = 0;
	int action = 0;
	int init_num_hosts = 0, init_max_hosts = DEFAULT_MAX_HOSTS;
	int rv, fd;

	to.host_timeout_seconds = 60;
	to.host_renewal_warn_seconds = 30;
	to.host_renewal_fail_seconds = 40;
	to.host_renewal_seconds = 10;
	to.script_shutdown_seconds = 10;
	to.sigterm_shutdown_seconds = 10;
	to.stable_poll_ms = 2000;
	to.unstable_poll_ms = 500;
	to.io_timeout_seconds = DEFAULT_IO_TIMEOUT_SECONDS;

	rv = read_args(argc, argv, &token_count, token_args,
		       &action, &init_num_hosts, &init_max_hosts);
	if (rv < 0)
		goto out;

	switch (action) {
	case ACT_DAEMON:
		rv = do_daemon();
		break;
	case ACT_INIT:
		rv = do_init(token_count, token_args,
			     init_num_hosts, init_max_hosts);
		break;

	/* client actions that ask daemon to do something.
	   we could split these into a separate command line
	   utility (note that the token arg processing is shared
	   between init and acquire.  It would also be a pain
	   to move init into a separate utility because it shares
	   disk paxos code with the daemon. */

	case ACT_COMMAND:
		fd = sm_register();
		if (fd < 0)
			goto out;
		rv = sm_acquire(fd, token_count, token_args);
		if (rv < 0)
			goto out;
		exec_command();
		break;
	case ACT_RELEASE:
		rv = sm_release(options.pid, token_count, token_args);
		break;
	case ACT_SHUTDOWN:
		rv = sm_shutdown();
		break;
	case ACT_STATUS:
		rv = sm_status();
		break;
	case ACT_LOG_DUMP:
		rv = sm_log_dump();
		break;
	case ACT_SET_HOST_ID:
		rv = sm_set_host_id(options.our_host_id);
		break;
	default:
		break;
	}
 out:
	return rv;
}

