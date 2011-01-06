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
#include <pthread.h>
#include <time.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>

#include "sanlock_internal.h"
#include "diskio.h"
#include "leader.h"
#include "log.h"
#include "watchdog.h"

/* TODO: why can a used host_id be acquired in D+6d (11 sec) instead of
 * waiting the full host_id_timeout_seconds (100) ? */

/*
 * Purpose of watchdog: to forcibly reset the host in the case where a
 * supervised pid is running but sanlock daemon does not renew its lease
 * and does not kill the pid (or it kills the pid but the pid does not
 * exit).  So, just before the pid begins running with granted leases,
 * watchdogd needs to be armed to reboot the host if things go bad right
 * after the pid goes ahead.
 *
 * The initial timestamp in the wd file should be set to the acquire_time
 * just before the daemon allows any pids to go ahead running with leases.
 *
 * If the daemon acquires its lease, creates the wd file containing
 * acquire_time, grants lease to a pid, fails to ever update the wd file,
 * and cannot kill the pid, watchdogd will reboot the host before
 * acquire_time + host_id_timeout_seconds, when another host could acquire
 * the lease.
 *
 * lease acquired at time AT
 * wd file created containing AT
 * pid starts running with granted lease
 * ...
 *
 * things go bad:
 * host_id_thread cannot renew lease
 * main thread cannot kill pid
 * watchdogd will reset host in AT + X seconds
 *
 * things go good:
 * host_id_thread renews lease at time RT
 * host_id_thread writes RT to wd file
 * watchdogd sees recent timestamp, pets wd device, host is not reset
 *
 * things go ok:
 * host_id_thread cannot renew lease
 * main thread kills pid
 * pid exits
 * stop_host_id unlinks wd file and stops host_id_thread
 * watchdogd does not see wd file with old time and does not reset host
 */

static char watchdog_path[PATH_MAX];
static int watchdog_fd;

static int do_write(int fd, void *buf, size_t count)
{
	int rv, off = 0;

 retry:
	rv = write(fd, (char *)buf + off, count);
	if (rv == -1 && errno == EINTR)
		goto retry;
	if (rv < 0) {
		return -errno;
	}

	if (rv != count) {
		count -= rv;
		off += rv;
		goto retry;
	}
	return 0;
}

void update_watchdog_file(uint64_t timestamp)
{
	char buf[16];

	if (!options.use_watchdog)
		return;

	memset(buf, 0, sizeof(buf));
	snprintf(buf, sizeof(buf), "%llu\n", (unsigned long long)timestamp);

	lseek(watchdog_fd, 0, SEEK_SET);

	do_write(watchdog_fd, buf, sizeof(buf));
}

int create_watchdog_file(uint64_t timestamp)
{
	char buf[16];
	int rv, fd;

	if (!options.use_watchdog)
		return 0;

	/* If this open fails with EEXIST I don't think it's safe to unlink
	 * watchdog_path and try again.  If the daemon had failed while pid's
	 * remained running, then the daemon is restarted (before watchdog
	 * triggers) and we start renewing host_id again and get here.  If we
	 * were to unlink the wd file right here, and then the daemon failed
	 * again, we'd possibly be left with pid's running that had been
	 * connected to the previous daemon instance, and the watchdog file
	 * unlinked, so the watchdog won't reset us.
	 *
	 * If the open fails with EEXIST we could open the existing file and go
	 * on, although there's currently no mechanism to reattach to any
	 * running pid's we're supposed to be supervising. */

	fd = open(watchdog_path, O_WRONLY|O_CREAT|O_EXCL|O_NONBLOCK, 0666);
	if (fd < 0) {
		log_error(NULL, "create_watchdog_file open %s error %d",
			  watchdog_path, errno);
		return fd;
	}

	memset(buf, 0, sizeof(buf));
	snprintf(buf, sizeof(buf), "%llu\n", (unsigned long long)timestamp);

	rv = do_write(fd, buf, sizeof(buf));
	if (rv < 0) {
		log_error(NULL, "create_watchdog_file write error %d", rv);
		close(fd);
		return rv;
	}

	watchdog_fd = fd;
	return 0;
}

void unlink_watchdog_file(void)
{
	if (!options.use_watchdog)
		return;

	unlink(watchdog_path);
	close(watchdog_fd);
}

int check_watchdog_file(void)
{
	struct stat buf;
	int rv;

	if (!options.use_watchdog)
		return 0;

	snprintf(watchdog_path, PATH_MAX, "%s/%s",
		 SANLK_RUN_DIR, SANLK_WATCHDOG_NAME);

	rv = stat(watchdog_path, &buf);

	if (rv == -1 && errno == ENOENT)
		return 0;

	log_error(NULL, "check watchdog file %s: %s",
		  watchdog_path, strerror(errno));

	return -errno;
}

