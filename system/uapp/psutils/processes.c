// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "processes.h"

#include <errno.h>
#include <inttypes.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <magenta/device/sysinfo.h>

static mx_status_t walk_process_tree_internal(job_callback_t job_callback, process_callback_t process_callback, mx_handle_t job, int depth) {
    mx_koid_t koids[128];
    size_t actual;
    size_t avail;
    mx_status_t status;

    // get the list of processes under this job
    status = mx_object_get_info(job, MX_INFO_JOB_PROCESSES, koids, sizeof(koids), &actual, &avail);
    // TODO: allocate a larger koids if 128 is not enough
    if (status != NO_ERROR) {
        return status;
    }

    for (size_t n = 0; n < actual; n++) {
        mx_handle_t child;
        status = mx_object_get_child(job, koids[n], MX_RIGHT_SAME_RIGHTS, &child);
        if (status == NO_ERROR) {
            // call the process_callback if supplied
            if (process_callback) {
                status = (process_callback)(depth, child, koids[n]);
                // abort on failure
                if (status != NO_ERROR) {
                    return status;
                }
            }

            mx_handle_close(child);
        }
    }

    // get a list of child jobs for this job
    status = mx_object_get_info(job, MX_INFO_JOB_CHILDREN, koids, sizeof(koids), &actual, &avail);
    // TODO: allocate a larger koids if 128 is not enough
    if (status != NO_ERROR) {
        return status;
    }

    // drill down into the job tree
    for (size_t n = 0; n < actual; n++) {
        mx_handle_t child;
        status = mx_object_get_child(job, koids[n], MX_RIGHT_SAME_RIGHTS, &child);
        if (status == NO_ERROR) {
            // call the job_callback if supplied
            if (job_callback) {
                status = (job_callback)(depth, child, koids[n]);
                // abort on failure
                if (status != NO_ERROR) {
                    return status;
                }
            }

            // recurse to its children
            status = walk_process_tree_internal(job_callback, process_callback, child, depth+1);
            // abort on failure
            if (status != NO_ERROR) {
                return status;
            }

            mx_handle_close(child);
        }
    }

    return NO_ERROR;

}

mx_status_t walk_process_tree(job_callback_t job_callback, process_callback_t process_callback) {
    int fd = open("/dev/misc/sysinfo", O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "ps: cannot open sysinfo: %d\n", errno);
        return ERR_NOT_FOUND;
    }
    mx_handle_t root_job;
    if (ioctl_sysinfo_get_root_job(fd, &root_job) != sizeof(root_job)) {
        fprintf(stderr, "ps: cannot obtain root job\n");
        return ERR_NOT_FOUND;
    }
    close(fd);

    return walk_process_tree_internal(job_callback, process_callback, root_job, 0);

    mx_handle_close(root_job);
}
