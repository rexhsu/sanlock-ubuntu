/*
 * Copyright 2010-2011 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __SANLOCK_ADMIN_H__
#define __SANLOCK_ADMIN_H__

/* add flags */
#define SANLK_ADD_ASYNC		0x00000001

/* rem flags */
#define SANLK_REM_ASYNC		0x00000001
#define SANLK_REM_UNUSED	0x00000002

/* inq flags */
#define SANLK_INQ_WAIT		0x00000001

/*
 * add_lockspace returns:
 * 0: the lockspace has been added successfully
 * -EEXIST: the lockspace already exists
 * -EINPROGRESS: the lockspace is already in the process of being added
 * (the in-progress add may or may not succeed)
 * -EAGAIN: the lockspace is being removed
 */

int sanlock_add_lockspace(struct sanlk_lockspace *ls, uint32_t flags);

/*
 * inq_lockspace returns:
 * 0: the lockspace exists and is currently held
 * -ENOENT: lockspace not found
 */

int sanlock_inq_lockspace(struct sanlk_lockspace *ls, uint32_t flags);

/*
 * rem_lockspace returns:
 * 0: the lockspace has been removed successfully
 * -EINPROGRESS: the lockspace is already in the process of being removed
 * -ENOENT: lockspace not found
 * -EBUSY: UNUSED was set and lockspace is being used
 *
 * The sanlock daemon will kill any pids using the lockspace when the
 * lockspace is removed (unless UNUSED is set).
 */

int sanlock_rem_lockspace(struct sanlk_lockspace *ls, uint32_t flags);

/*
 * Returns the alignment in bytes required by sanlock_init()
 * (1MB for disks with 512 sectors, 8MB for disks with 4096 sectors)
 */

int sanlock_align(struct sanlk_disk *disk);

/*
 * Ask sanlock daemon to initialize disk space.
 * Use max_hosts = 0 for default value.
 * Use num_hosts = 0 for default value.
 * Provide either lockspace or resource, not both
 */

int sanlock_init(struct sanlk_lockspace *ls,
		 struct sanlk_resource *res,
		 int max_hosts, int num_hosts);

#endif
