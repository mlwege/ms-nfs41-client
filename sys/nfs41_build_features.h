/* NFSv4.1 client for Windows
 * Copyright � 2012 The Regents of the University of Michigan
 *
 * Olga Kornievskaia <aglo@umich.edu>
 * Casey Bodley <cbodley@umich.edu>
 * Roland Mainz <roland.mainz@nrubsig.org>
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

#ifndef _NFS41_DRIVER_BUILDFEATURES_
#define _NFS41_DRIVER_BUILDFEATURES_ 1

/*
 * NFS41_DRIVER_FEATURE_* - features for this build, we use this
 * for development to add new features which are "off" by default
 * until they are ready
 */

/*
 * NFS41_DRIVER_FEATURE_LOCAL_UIDGID_IN_NFSV3ATTRIBUTES - return local uid/gid values
 */
// #define NFS41_DRIVER_FEATURE_LOCAL_UIDGID_IN_NFSV3ATTRIBUTES 1

/*
 * NFS41_DRIVER_FEATURE_MAP_UNMAPPED_USER_TO_UNIXUSER_SID - give NFS
 * files which do not map to a local account a SID in the
 * Unix_User+x/Unix_Group+x range
 */
// #define NFS41_DRIVER_FEATURE_MAP_UNMAPPED_USER_TO_UNIXUSER_SID 1

/*
 * NFS41_DRIVER_FEATURE_NAMESERVICE_CYGWIN - use Cygwin /usr/bin/getent
 * as "name service"
 */
// #define NFS41_DRIVER_FEATURE_NAMESERVICE_CYGWIN 1

#endif /* !_NFS41_DRIVER_BUILDFEATURES_ */
