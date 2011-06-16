/* Copyright (c) 2010
 * The Regents of the University of Michigan
 * All Rights Reserved
 *
 * Permission is granted to use, copy and redistribute this software
 * for noncommercial education and research purposes, so long as no
 * fee is charged, and so long as the name of the University of Michigan
 * is not used in any advertising or publicity pertaining to the use
 * or distribution of this software without specific, written prior
 * authorization.  Permission to modify or otherwise create derivative
 * works of this software is not granted.
 *
 * This software is provided as is, without representation or warranty
 * of any kind either express or implied, including without limitation
 * the implied warranties of merchantability, fitness for a particular
 * purpose, or noninfringement.  The Regents of the University of
 * Michigan shall not be liable for any damages, including special,
 * indirect, incidental, or consequential damages, with respect to any
 * claim arising out of or in connection with the use of the software,
 * even if it has been or is hereafter advised of the possibility of
 * such damages.
 */

#include <Windows.h>
#include <strsafe.h>
#include <stdio.h>

#include "nfs41_ops.h"
#include "nfs41_callback.h"
#include "daemon_debug.h"


#define FDLVL 2 /* dprintf level for file device logging */


/* pnfs_file_device_list */
struct pnfs_file_device_list {
    struct list_entry       head;
    CRITICAL_SECTION        lock;
};

#define device_entry(pos) list_container(pos, pnfs_file_device, entry)


static enum pnfs_status file_device_create(
    IN const unsigned char *deviceid,
    IN struct pnfs_file_device_list *devices,
    OUT pnfs_file_device **device_out)
{
    enum pnfs_status status = PNFS_SUCCESS;
    pnfs_file_device *device;

    device = calloc(1, sizeof(pnfs_file_device));
    if (device == NULL) {
        status = PNFSERR_RESOURCES;
        goto out;
    }

    memcpy(device->device.deviceid, deviceid, PNFS_DEVICEID_SIZE);
    device->devices = devices;
    InitializeCriticalSection(&device->device.lock);
    *device_out = device;
out:
    return status;
}

static void file_device_free(
    IN pnfs_file_device *device)
{
    free(device->servers.arr);
    free(device->stripes.arr);
    free(device);
}

static int deviceid_compare(
    const struct list_entry *entry,
    const void *deviceid)
{
    const pnfs_file_device *device = device_entry(entry);
    return memcmp(device->device.deviceid, deviceid, PNFS_DEVICEID_SIZE);
}

static enum pnfs_status file_device_find_or_create(
    IN const unsigned char *deviceid,
    IN struct pnfs_file_device_list *devices,
    OUT pnfs_file_device **device_out)
{
    struct list_entry *entry;
    enum pnfs_status status;

    dprintf(FDLVL, "--> pnfs_file_device_find_or_create()\n");

    EnterCriticalSection(&devices->lock);

    /* search for an existing device */
    entry = list_search(&devices->head, deviceid, deviceid_compare);
    if (entry == NULL) {
        /* create a new device */
        pnfs_file_device *device;
        status = file_device_create(deviceid, devices, &device);
        if (status == PNFS_SUCCESS) {
            /* add it to the list */
            list_add_tail(&devices->head, &device->entry);
            *device_out = device;

            dprintf(FDLVL, "<-- pnfs_file_device_find_or_create() "
                "returning new device %p\n", device);
        } else {
            dprintf(FDLVL, "<-- pnfs_file_device_find_or_create() "
                "returning %s\n", pnfs_error_string(status));
        }
    } else {
        *device_out = device_entry(entry);
        status = PNFS_SUCCESS;

        dprintf(FDLVL, "<-- pnfs_file_device_find_or_create() "
            "returning existing device %p\n", *device_out);
    }

    LeaveCriticalSection(&devices->lock);
    return status;
}


enum pnfs_status pnfs_file_device_list_create(
    OUT struct pnfs_file_device_list **devices_out)
{
    enum pnfs_status status = PNFS_SUCCESS;
    struct pnfs_file_device_list *devices;

    devices = calloc(1, sizeof(struct pnfs_file_device_list));
    if (devices == NULL) {
        status = PNFSERR_RESOURCES;
        goto out;
    }

    list_init(&devices->head);
    InitializeCriticalSection(&devices->lock);

    *devices_out = devices;
out:
    return status;
}

void pnfs_file_device_list_free(
    IN struct pnfs_file_device_list *devices)
{
    struct list_entry *entry, *tmp;

    EnterCriticalSection(&devices->lock);

    list_for_each_tmp(entry, tmp, &devices->head)
        file_device_free(device_entry(entry));

    LeaveCriticalSection(&devices->lock);

    free(devices);
}

void pnfs_file_device_list_invalidate(
    IN struct pnfs_file_device_list *devices)
{
    struct list_entry *entry, *tmp;
    pnfs_file_device *device;

    dprintf(FDLVL, "--> pnfs_file_device_list_invalidate()\n");

    EnterCriticalSection(&devices->lock);

    list_for_each_tmp(entry, tmp, &devices->head) {
        device = device_entry(entry);
        EnterCriticalSection(&device->device.lock);
        /* if there are layouts still using the device, flag it
         * as revoked and clean up on last reference */
        if (device->device.layout_count) {
            device->device.status |= PNFS_DEVICE_REVOKED;
            LeaveCriticalSection(&device->device.lock);
        } else {
            LeaveCriticalSection(&device->device.lock);
            /* no layouts are using it, so it's safe to free */
            list_remove(entry);
            file_device_free(device);
        }
    }

    LeaveCriticalSection(&devices->lock);

    dprintf(FDLVL, "<-- pnfs_file_device_list_invalidate()\n");
}


/* pnfs_file_device */
enum pnfs_status pnfs_file_device_get(
    IN nfs41_session *session,
    IN struct pnfs_file_device_list *devices,
    IN unsigned char *deviceid,
    OUT pnfs_file_device **device_out)
{
    pnfs_file_device *device;
    enum pnfs_status status;
    enum nfsstat4 nfsstat;

    dprintf(FDLVL, "--> pnfs_file_device_get()\n");

    status = file_device_find_or_create(deviceid, devices, &device);
    if (status)
        goto out;

    EnterCriticalSection(&device->device.lock);

    /* don't give out a device that's been revoked */
    if (device->device.status & PNFS_DEVICE_REVOKED)
        status = PNFSERR_NO_DEVICE;
    else if (device->device.status & PNFS_DEVICE_GRANTED)
        status = PNFS_SUCCESS;
    else {
        nfsstat = pnfs_rpc_getdeviceinfo(session, deviceid, device);
        if (nfsstat == NFS4_OK) {
            device->device.status = PNFS_DEVICE_GRANTED;
            status = PNFS_SUCCESS;

            dprintf(FDLVL, "Received device info:\n");
            dprint_device(FDLVL, device);
        } else {
            status = PNFSERR_NO_DEVICE;

            eprintf("pnfs_rpc_getdeviceinfo() failed with %s\n",
                nfs_error_string(nfsstat));
        }
    }

    if (status == PNFS_SUCCESS) {
        device->device.layout_count++;
        dprintf(FDLVL, "pnfs_file_device_get() -> %u\n",
            device->device.layout_count);
        *device_out = device;
    }

    LeaveCriticalSection(&device->device.lock);
out:
    dprintf(FDLVL, "<-- pnfs_file_device_get() returning %s\n",
        pnfs_error_string(status));
    return status;
}

void pnfs_file_device_put(
    IN pnfs_file_device *device)
{
    uint32_t count;
    EnterCriticalSection(&device->device.lock);
    count = --device->device.layout_count;
    dprintf(FDLVL, "pnfs_file_device_put() -> %u\n", count);

    /* if the device was revoked, remove/free the device on last reference */
    if (count == 0 && device->device.status & PNFS_DEVICE_REVOKED) {
        EnterCriticalSection(&device->devices->lock);
        list_remove(&device->entry);
        LeaveCriticalSection(&device->devices->lock);

        LeaveCriticalSection(&device->device.lock);

        file_device_free(device);
        dprintf(FDLVL, "revoked file device freed after last reference\n");
    } else {
        LeaveCriticalSection(&device->device.lock);
    }
}

static enum pnfs_status data_client_status(
    IN pnfs_data_server *server,
    OUT nfs41_client **client_out)
{
    enum pnfs_status status = PNFSERR_NOT_CONNECTED;

    if (server->client) {
        dprintf(FDLVL, "pnfs_data_server_client() returning "
            "existing client %llu\n", server->client->clnt_id);
        *client_out = server->client;
        status = PNFS_SUCCESS;
    }
    return status;
}

enum pnfs_status pnfs_data_server_client(
    IN nfs41_root *root,
    IN pnfs_data_server *server,
    IN uint32_t default_lease,
    OUT nfs41_client **client_out)
{
    int status;
    enum pnfs_status pnfsstat;

    dprintf(FDLVL, "--> pnfs_data_server_client('%s')\n",
        server->addrs.arr[0].uaddr);

    /* if we've already created the client, return it */
    AcquireSRWLockShared(&server->lock);
    pnfsstat = data_client_status(server, client_out);
    ReleaseSRWLockShared(&server->lock);

    if (pnfsstat) {
        AcquireSRWLockExclusive(&server->lock);

        pnfsstat = data_client_status(server, client_out);
        if (pnfsstat) {
            status = nfs41_root_mount_addrs(root, &server->addrs,
                1, default_lease, &server->client);
            if (status) {
                dprintf(FDLVL, "data_client_create('%s') failed with %d\n",
                    server->addrs.arr[0].uaddr, status);
            } else {
                *client_out = server->client;
                pnfsstat = PNFS_SUCCESS;

                dprintf(FDLVL, "pnfs_data_server_client() returning "
                    "new client %llu\n", server->client->clnt_id);
            }
        }

        ReleaseSRWLockExclusive(&server->lock);
    }
    return pnfsstat;
}


/* 13.4.2. Interpreting the File Layout Using Sparse Packing
 * http://tools.ietf.org/html/rfc5661#section-13.4.2 */

static enum pnfs_status get_sparse_fh(
    IN pnfs_io_pattern *pattern,
    IN uint32_t stripeid,
    OUT nfs41_path_fh **file_out)
{
    pnfs_file_layout *layout = pattern->layout;
    const uint32_t filehandle_count = layout->filehandles.count;
    const uint32_t server_count = layout->device->servers.count;
    enum pnfs_status status = PNFS_SUCCESS;

    if (filehandle_count == server_count) {
        const uint32_t serverid = data_server_index(layout->device, stripeid);
        *file_out = &layout->filehandles.arr[serverid];
    } else if (filehandle_count == 1) {
        *file_out = &layout->filehandles.arr[0];
    } else if (filehandle_count == 0) {
        *file_out = pattern->meta_file;
    } else {
        eprintf("invalid sparse layout! has %u file handles "
            "and %u servers\n", filehandle_count, server_count);
        status = PNFSERR_INVALID_FH_LIST;
    }
    return status;
}

/* 13.4.3. Interpreting the File Layout Using Dense Packing
* http://tools.ietf.org/html/rfc5661#section-13.4.3 */

static enum pnfs_status get_dense_fh(
    IN pnfs_io_pattern *pattern,
    IN uint32_t stripeid,
    OUT nfs41_path_fh **file_out)
{
    pnfs_file_layout *layout = pattern->layout;
    const uint32_t filehandle_count = layout->filehandles.count;
    const uint32_t stripe_count = layout->device->stripes.count;
    enum pnfs_status status = PNFS_SUCCESS;

    if (filehandle_count == stripe_count) {
        *file_out = &layout->filehandles.arr[stripeid];
    } else {
        eprintf("invalid dense layout! has %u file handles "
            "and %u stripes\n", filehandle_count, stripe_count);
        status = PNFSERR_INVALID_FH_LIST;
    }
    return status;
}

static __inline uint64_t positive_remainder(
    IN uint64_t dividend,
    IN uint32_t divisor)
{
    const uint64_t remainder = dividend % divisor;
    return remainder < divisor ? remainder : remainder + divisor;
}

/* 13.4.4. Sparse and Dense Stripe Unit Packing
 * http://tools.ietf.org/html/rfc5661#section-13.4.4 */

enum pnfs_status pnfs_file_device_io_unit(
    IN pnfs_io_pattern *pattern,
    IN uint64_t offset,
    OUT pnfs_io_unit *io)
{
    pnfs_file_layout *layout = pattern->layout;
    enum pnfs_status status = PNFS_SUCCESS;

    const uint32_t unit_size = layout_unit_size(layout);
    const uint32_t stripe_count = layout->device->stripes.count;
    const uint64_t sui = stripe_unit_number(layout, offset, unit_size);
    const uint64_t offset_end = layout->pattern_offset + unit_size * (sui + 1);

    io->stripeid = stripe_index(layout, sui, stripe_count);
    io->serverid = data_server_index(layout->device, io->stripeid);

    if (is_dense(layout)) {
        const uint64_t rel_offset = offset - layout->pattern_offset;
        const uint64_t remainder = positive_remainder(rel_offset, unit_size);
        const uint32_t stride = unit_size * stripe_count;

        io->offset = (rel_offset / stride) * unit_size + remainder;

        status = get_dense_fh(pattern, io->stripeid, &io->file);
    } else {
        io->offset = offset;

        status = get_sparse_fh(pattern, io->stripeid, &io->file);
    }

    io->buffer = pattern->buffer + offset - pattern->offset_start;
    io->length = offset_end - offset;
    if (offset + io->length > pattern->offset_end)
        io->length = pattern->offset_end - offset;
    return status;
}


/* CB_NOTIFY_DEVICEID */
enum pnfs_status pnfs_file_device_notify(
    IN struct pnfs_file_device_list *devices,
    IN const struct notify_deviceid4 *change)
{
    struct list_entry *entry;
    enum pnfs_status status = PNFSERR_NO_DEVICE;

    dprintf(FDLVL, "--> pnfs_file_device_notify(%u, %0llX:%0llX)\n",
        change->type, change->deviceid);

    if (change->layouttype != PNFS_LAYOUTTYPE_FILE) {
        status = PNFSERR_NOT_SUPPORTED;
        goto out;
    }

    EnterCriticalSection(&devices->lock);

    entry = list_search(&devices->head, change->deviceid, deviceid_compare);
    if (entry) {
        dprintf(FDLVL, "found file device %p\n", device_entry(entry));

        if (change->type == NOTIFY_DEVICEID4_CHANGE) {
            /* if (change->immediate) ... */
            dprintf(FDLVL, "CHANGE (%u)\n", change->immediate);
        } else if (change->type == NOTIFY_DEVICEID4_DELETE) {
            /* This notification MUST NOT be sent if the client
             * has a layout that refers to the device ID. */
            dprintf(FDLVL, "DELETE\n");
        }
        status = PNFS_SUCCESS;
    }

    LeaveCriticalSection(&devices->lock);
out:
    dprintf(FDLVL, "<-- pnfs_file_device_notify() returning %s\n",
        pnfs_error_string(status));
    return status;
}
