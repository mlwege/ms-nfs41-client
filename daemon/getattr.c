/* NFSv4.1 client for Windows
 * Copyright � 2012 The Regents of the University of Michigan
 *
 * Olga Kornievskaia <aglo@umich.edu>
 * Casey Bodley <cbodley@umich.edu>
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * without any warranty; without even the implied warranty of merchantability
 * or fitness for a particular purpose.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 */

#include <Windows.h>
#include <stdio.h>
#include <strsafe.h>
#include "from_kernel.h"
#include "delegation.h"
#include "daemon_debug.h"
#include "nfs41_ops.h"
#include "name_cache.h"
#include "upcall.h"
#include "util.h"


int nfs41_cached_getattr(
    IN nfs41_session *session,
    IN nfs41_path_fh *file,
    OUT nfs41_file_info *info)
{
    int status;

    /* first look for cached attributes */
    status = nfs41_attr_cache_lookup(session_name_cache(session),
        file->fh.fileid, info);

    if (status) {
        /* fetch attributes from the server */
        bitmap4 attr_request;
        nfs41_superblock_getattr_mask(file->fh.superblock, &attr_request);

        status = nfs41_getattr(session, file, &attr_request, info);
        if (status) {
            eprintf("nfs41_getattr() failed with %s\n",
                nfs_error_string(status));
            status = nfs_to_windows_error(status, ERROR_BAD_NET_RESP);
        }
    }
    return status;
}

/* NFS41_FILE_QUERY */
static int parse_getattr(unsigned char *buffer, uint32_t length, nfs41_upcall *upcall)
{
    int status;
    getattr_upcall_args *args = &upcall->args.getattr;

    status = safe_read(&buffer, &length, &args->query_class, sizeof(args->query_class));
    if (status) goto out;
    status = safe_read(&buffer, &length, &args->buf_len, sizeof(args->buf_len));
    if (status) goto out;

    dprintf(1, "parsing NFS41_FILE_QUERY: info_class=%d buf_len=%d file=%.*s\n",
        args->query_class, args->buf_len, upcall->state_ref->path.len, 
        upcall->state_ref->path.path);
out:
    return status;
}

static int handle_getattr(nfs41_upcall *upcall)
{
    int status;
    getattr_upcall_args *args = &upcall->args.getattr;
    nfs41_open_state *state = upcall->state_ref;
    nfs41_file_info info = { 0 };

    status = nfs41_cached_getattr(state->session, &state->file, &info);
    if (status) {
        eprintf("nfs41_cached_getattr() failed with %d\n", status);
        goto out;
    }

    if (info.type == NF4LNK) {
        nfs41_file_info target_info;
        int target_status = nfs41_symlink_follow(upcall->root_ref,
            state->session, &state->file, &target_info);
        if (target_status == NO_ERROR && target_info.type == NF4DIR)
            info.symlink_dir = TRUE;
    }

    switch (args->query_class) {
    case FileBasicInformation:
        nfs_to_basic_info(&info, &args->basic_info);
        args->ctime = info.change;
        break;
    case FileStandardInformation:
        nfs_to_standard_info(&info, &args->std_info);
        break;
    case FileAttributeTagInformation:
        args->tag_info.FileAttributes = nfs_file_info_to_attributes(&info);
        args->tag_info.ReparseTag = info.type == NF4LNK ?
            IO_REPARSE_TAG_SYMLINK : 0;
        break;
    case FileInternalInformation:
        args->intr_info.IndexNumber.QuadPart = info.fileid;
        break;
    case FileNetworkOpenInformation:
        nfs_to_network_openinfo(&info, &args->network_info);
        break;
    default:
        eprintf("unhandled file query class %d\n", args->query_class);
        status = ERROR_INVALID_PARAMETER;
        break;
    }
out:
    return status;
}

static int marshall_getattr(unsigned char *buffer, uint32_t *length, nfs41_upcall *upcall)
{
    int status;
    getattr_upcall_args *args = &upcall->args.getattr;
    uint32_t info_len;

    switch (args->query_class) {
    case FileBasicInformation:
        info_len = sizeof(args->basic_info);
        status = safe_write(&buffer, length, &info_len, sizeof(info_len));
        if (status) goto out;
        status = safe_write(&buffer, length, &args->basic_info, info_len);
        if (status) goto out;
        break;
    case FileStandardInformation:
        info_len = sizeof(args->std_info);
        status = safe_write(&buffer, length, &info_len, sizeof(info_len));
        if (status) goto out;
        status = safe_write(&buffer, length, &args->std_info, info_len);
        if (status) goto out;
        break;
    case FileAttributeTagInformation:
        info_len = sizeof(args->tag_info);
        status = safe_write(&buffer, length, &info_len, sizeof(info_len));
        if (status) goto out;
        status = safe_write(&buffer, length, &args->tag_info, info_len);
        if (status) goto out;
        break;
    case FileInternalInformation:
        info_len = sizeof(args->intr_info);
        status = safe_write(&buffer, length, &info_len, sizeof(info_len));
        if (status) goto out;
        status = safe_write(&buffer, length, &args->intr_info, info_len);
        if (status) goto out;
        break;
    case FileNetworkOpenInformation:
        info_len = sizeof(args->network_info);
        status = safe_write(&buffer, length, &info_len, sizeof(info_len));
        if (status) goto out;
        status = safe_write(&buffer, length, &args->network_info, info_len);
        if (status) goto out;
        break;
    default:
        eprintf("unknown file query class %d\n", args->query_class);
        status = 103;
        goto out;
    }
    status = safe_write(&buffer, length, &args->ctime, sizeof(args->ctime));
    if (status) goto out;
    dprintf(1, "NFS41_FILE_QUERY: downcall changattr=%llu\n", args->ctime);
out:
    return status;
}


static int parse_getexattr(unsigned char *buffer, uint32_t length, nfs41_upcall *upcall)
{
    int status;
    getexattr_upcall_args *args = &upcall->args.getexattr;

    status = get_name(&buffer, &length, &args->path);
    if (status) goto out;
    status = safe_read(&buffer, &length, &args->eaindex, sizeof(args->eaindex));
    if (status) goto out;
    status = safe_read(&buffer, &length, &args->restart, sizeof(args->restart));
    if (status) goto out;
    status = safe_read(&buffer, &length, &args->single, sizeof(args->single));
    if (status) goto out;
    status = safe_read(&buffer, &length, &args->ealist_len, sizeof(args->ealist_len));
    if (status) goto out;
    args->ealist = buffer;

    dprintf(1, "parsing NFS41_EA_QUERY: buf_len=%d Initial %d Restart %d "
        "Single %d\n", args->buf_len,args->eaindex, args->restart, args->single);
out:
    return status;    
}

static int marshall_getexattr(unsigned char *buffer, uint32_t *length, nfs41_upcall *upcall)
{
    int status = NO_ERROR;
    getexattr_upcall_args *args = &upcall->args.getexattr;
    uint32_t len = args->buf_len;

    status = safe_write(&buffer, length, &len, sizeof(len));
    if (status) goto out;
    status = safe_write(&buffer, length, args->buf, len);
    if (status) goto out;
out:
    free(args->buf);
    return status;
}


static int handle_getexattr(nfs41_upcall *upcall)
{
    int status = 0;
    getexattr_upcall_args *args = &upcall->args.getexattr;
    PFILE_GET_EA_INFORMATION gea = 
        (PFILE_GET_EA_INFORMATION)args->ealist, prev = NULL;
    PFILE_FULL_EA_INFORMATION eainfo, entry_pos;
    unsigned char *entry_buf, buf[NFS4_EASIZE] = { 0 };
    nfs41_open_state *state = upcall->state_ref;
    nfs41_path_fh parent = { 0 }, file = { 0 };
    open_claim4 claim;
    stateid4 open_stateid;
    stateid_arg stateid;
    open_delegation4 delegation = { 0 };
    bool_t eof;
    uint32_t bytes_read = 0;
    ULONG buflen = 0, needed = 0;	
            
    status = nfs41_rpc_openattr(state->session, &state->file, FALSE, &parent.fh);
    if (status){
        dprintf(1, "nfs41_rpc_openattr() failed with error %s.\n",
            nfs_error_string(status));
        status = nfs_to_windows_error(status, ERROR_NOT_SUPPORTED);
        goto out;
    }

    entry_buf = malloc(UPCALL_BUF_SIZE);
    if (entry_buf == NULL) {
        status = GetLastError();
        goto out;
    }

    entry_pos = eainfo = (PFILE_FULL_EA_INFORMATION)entry_buf;

    while (gea != prev) {
        file.name.name = gea->EaName;
        file.name.len = gea->EaNameLength;
        claim.claim = CLAIM_NULL;
        claim.u.null.filename = &file.name;
        status = nfs41_open(state->session, &parent, &file, &state->owner, 
            &claim, OPEN4_SHARE_ACCESS_READ, OPEN4_SHARE_DENY_BOTH, 
            OPEN4_NOCREATE, UNCHECKED4, 0, TRUE, &open_stateid, 
            &delegation, NULL);
        if (status) {
            dprintf(1, "nfs41_open() failed with error %s.\n",
                nfs_error_string(status));
            status = nfs_to_windows_error(status, ERROR_NOT_SUPPORTED);
            goto out_free;
        }
        
        stateid.stateid = open_stateid;
        stateid.stateid.seqid = 0;
        status = nfs41_read(state->session, &file, &stateid, 0, NFS4_EASIZE, 
                buf, &bytes_read, &eof);
        if (status) {
            dprintf(2, "nfs41_rpc_read EA attribute failed\n");
            status = nfs_to_windows_error(status, ERROR_NET_WRITE_FAULT);
            nfs41_close(state->session, &file, &stateid);
            goto out_free;
        }

        if (eof) {
            dprintf(1, "read thread reached eof: bytes_read %d\n", bytes_read);
            eainfo->EaNameLength = gea->EaNameLength;
            if (FAILED(StringCchCopy((LPSTR)eainfo->EaName, gea->EaNameLength + 1,
                    (LPCSTR)gea->EaName))) {
                status = ERROR_BUFFER_OVERFLOW;
                nfs41_close(state->session, &file, &stateid);
                goto out_free;
            }

            if (FAILED(StringCchCopy((LPSTR)eainfo->EaName + 
                    eainfo->EaNameLength + 1, bytes_read + 1, (LPCSTR)buf))) {
                status = ERROR_BUFFER_OVERFLOW;
                nfs41_close(state->session, &file, &stateid);
                goto out_free;
            }
    
            memset(buf, 0, NFS4_EASIZE);
            eainfo->EaValueLength = (USHORT) bytes_read;
            needed = (eainfo->EaNameLength + eainfo->EaValueLength) +
                FIELD_OFFSET(FILE_FULL_EA_INFORMATION, EaName);

            if (needed % 4)
                needed = needed + (4 - (needed % 4)); 

            eainfo->NextEntryOffset = needed;
            eainfo->Flags = 0;

            buflen = buflen + needed;
            prev = gea;
            
            if (gea->NextEntryOffset != 0) {
                gea = (PFILE_GET_EA_INFORMATION) 
                    ((PBYTE) gea + gea->NextEntryOffset); 
                eainfo = (PFILE_FULL_EA_INFORMATION) 
                    ((PBYTE) eainfo + eainfo->NextEntryOffset);
            }

            status = nfs41_close(state->session, &file, &stateid);
            if (status) {
                dprintf(1, "nfs41_close() failed with error %s.\n",
                    nfs_error_string(status));
                status = nfs_to_windows_error(status, ERROR_NOT_SUPPORTED);
                goto out_free;
            }
        } else {
            dprintf(2, "Size of the EA value is greater than %d\n", NFS4_EASIZE);
            status = nfs41_close(state->session, &file, &stateid);
            if (status) {
                dprintf(1, "nfs41_rpc_openattr() failed with error %s.\n",
                    nfs_error_string(status));
                status = nfs_to_windows_error(status, ERROR_NOT_SUPPORTED);               
            }
            /* treating extended attribute values larger than NFS4_EASIZE as failure */
            status = ERROR_INVALID_DATA;
            goto out_free;

        }
    }

    eainfo->NextEntryOffset = 0;
    args->buf = (unsigned char *)entry_pos;
    args->buf_len = buflen;
    goto out;

out_free:
    free(entry_buf);
out:
    return status;

}



const nfs41_upcall_op nfs41_op_getattr = {
    parse_getattr,
    handle_getattr,
    marshall_getattr
};

const nfs41_upcall_op nfs41_op_getexattr = {
    parse_getexattr,
    handle_getexattr,
    marshall_getexattr
    };