#ifndef __SM_H__
#define __SM_H__

#ifndef GNUC_UNUSED
#define GNUC_UNUSED __attribute__((__unused__))
#endif

#ifndef EXTERN
#define EXTERN extern
#else
#undef EXTERN
#define EXTERN
#endif

#define PAXOS_DISK_MAGIC 0x06152010
#define PAXOS_DISK_VERSION_MAJOR 0x00020000
#define PAXOS_DISK_VERSION_MINOR 0x00000001

#define SM_MAGIC 0x04282010

/* max leases that sm will manage */

#define MAX_LEASES 8

/* max disks in a single lease */

#define MAX_DISKS 8

/* max leases on the command line */

#define MAX_LEASE_ARGS 50

/* includes terminating null byte */

#define DISK_PATH_LEN 1024

/* default num_hosts and host_id range */

#define DEFAULT_MAX_HOSTS 2000

/* does not include terminating null byte */

#define NAME_ID_SIZE 48

#define SM_LOG_DUMP_SIZE (1024*1024)

/* this is just the path to the executable, not full command line */

#define COMMAND_MAX 4096

#define SM_RUN_DIR "/var/run/sync_manager"
#define SM_LOG_DIR "/var/log/sync_manager"
#define DAEMON_WATCHDOG_DIR "/var/run/sync_manager/watchdog"
#define DAEMON_SOCKET_DIR "/var/run/sync_manager/sockets"
#define DAEMON_LOCKFILE_DIR "/var/run/sync_manager/daemon_lockfiles/"
#define RESOURCE_LOCKFILE_DIR "/var/run/sync_manager/resource_lockfiles/"

#define MAIN_SOCKET_NAME "main"

#define DAEMON_NAME "daemon"

#define SMERR_UNREGISTERED -501;

/*
 * io_timeout_seconds - max time a single disk read or write can take to return
 * (if -1 then non-async i/o is used and time is unlimited)
 * (cf. safelease.c "max_op_ms")
 * (cf. light weight leases paper small delta)
 * (cf. delta_lease.c log message "d")
 *
 * host_id_renewal_seconds - attempt a renewal once in each interval of this length
 * (the sleeping time between each attempt will be this time minus the time spent
 * in renewal) 
 *
 * host_id_renewal_fail_seconds - daemon must renew lease once within this time
 * period to keep the lease.  daemon enters recovery mode (kills supervised pids)
 * if the lease is not renewed in this interval.
 * (cf. safelease.c "lease_ms")
 * (cf. light weight leases paper large delta)
 * (cf. delta_lease.c log message "D")
 *
 * host_id_timeout_seconds - one host considers another dead if the other's
 * host_id lease is this old (or more), and will take ownership of any resource
 * leases that the other host owned.
 *
 * Example of how host_id_timeout_seconds is derived (primarily from 
 * host_id_renewal_fail_seconds).
 * . host_id_renewal_fail_seconds 30
 * . host_id_timeout_seconds 100
 *
 * - lease ages to 30 sec at which point daemon enters recovery mode
 *   (kills pids) and stops updating wd file
 * - 60 more seconds after we stop updating wd file, the wd fires
 *   (assuming standard 60 wd timeout, and assuming we don't unlink wd file first)
 * - this 90 seconds isn't quite right because of the intervals between
 *   checks (daemon checking lease age, and watchdog daemon running checks), so
 *   add 10 more seconds to deal with check intervals, e.g. the watchdog daemon
 *   checks the status every 5 or 10 seconds, so the lease may be 30-40 sec
 *   old when it stops petting the wd device
 */

#define DEFAULT_IO_TIMEOUT_SECONDS 1
#define DEFAULT_HOST_ID_RENEWAL_SECONDS 5
#define DEFAULT_HOST_ID_RENEWAL_FAIL_SECONDS 30
#define DEFAULT_HOST_ID_TIMEOUT_SECONDS 100

struct sm_timeouts {
	int io_timeout_seconds;
	int host_id_timeout_seconds;
	int host_id_renewal_seconds;
	int host_id_renewal_fail_seconds;
};

struct sm_options {
	int use_aio;
	int use_watchdog;
	int our_host_id;
	uint32_t cluster_mode;
	int pid;
	int host_id;
	int incoming;
	char host_id_path[DISK_PATH_LEN];
	int host_id_offset;
};

EXTERN struct sm_options options;
EXTERN struct sm_timeouts to;

#endif

