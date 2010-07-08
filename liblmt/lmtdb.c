/*****************************************************************************
 *  Copyright (C) 2007-2010 Lawrence Livermore National Security, LLC.
 *  This module written by Jim Garlick <garlick@llnl.gov>.
 *  UCRL-CODE-232438
 *  All Rights Reserved.
 *
 *  This file is part of Lustre Monitoring Tool, version 2.
 *  Authors: H. Wartens, P. Spencer, N. O'Neill, J. Long, J. Garlick
 *  For details, see http://code.google.com/p/lmt/.
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License (as published by the
 *  Free Software Foundation) version 2, dated June 1991.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the IMPLIED WARRANTY OF MERCHANTABILITY
 *  or FITNESS FOR A PARTICULAR PURPOSE. See the terms and conditions of the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software Foundation,
 *  Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA or see
 *  <http://www.gnu.org/licenses/>.
 *****************************************************************************/

/* lmtdb.c - wire up mysql.c and ost|mdt|router.c for use by cerebro monitor */

#if HAVE_CONFIG_H
#include "config.h"
#endif /* HAVE_CONFIG_H */

#include <stdio.h>
#include <stdlib.h>
#if STDC_HEADERS
#include <string.h>
#endif /* STDC_HEADERS */
#include <errno.h>
#include <sys/utsname.h>
#include <stdint.h>
#include <sys/time.h>

#include "list.h"
#include "hash.h"

#include "proc.h"
#include "lmt.h"

#include "ost.h"
#include "mdt.h"
#include "router.h"
#include "lmtmysql.h"

/**
 ** Manage a list of db handles.
 **/

static List dbs = NULL;
static struct timeval last_connect = { .tv_usec = 0, .tv_sec = 0 };

/* default to localhost:3306 root:"" */
static const char *db_host = NULL;
static const unsigned int db_port = 0;
static const char *db_user = NULL;
static const char *db_passwd = NULL;

#define MIN_RECONNECT_SECS  15

static int
_init_db_ifneeded (const char **errp, int *retp)
{
    int retval = -1;
    struct timeval now;

    if (!dbs) {
        if (gettimeofday (&now, NULL) < 0)
            goto done;
        if (now.tv_sec - last_connect.tv_sec < MIN_RECONNECT_SECS) {
            *retp = 0; /* silently succeed: too early to reconnect */
            goto done;
        }
        last_connect = now;
        if (lmt_db_create_all (db_host, db_port, db_user, db_passwd,
                               &dbs, errp) < 0)
            goto done;
    }
    retval = 0;
done:
    return retval;
}

static void
_trigger_db_reconnect (void)
{
    if (dbs) {
        list_destroy (dbs);
        dbs = NULL;
    }
}

/**
 ** Handlers for incoming strings.
 **/

/* Helper for lmt_db_insert_ost_v2 () */
static int
_insert_ostinfo (char *s, const char **errp)
{
    int retval = -1;
    ListIterator itr = NULL;
    lmt_db_t db;
    char *name = NULL;
    uint64_t read_bytes, write_bytes;
    uint64_t kbytes_free, kbytes_total;
    uint64_t inodes_free, inodes_total;
    int inserts = 0;

    if (lmt_ost_decode_v2_ostinfo (s, &name, &read_bytes, &write_bytes,
                                    &kbytes_free, &kbytes_total,
                                    &inodes_free, &inodes_total) < 0) {
        if (errno == EIO)
            *errp = "error parsing ost_v2 string";
        goto done;
    }
    if (!(itr = list_iterator_create (dbs)))
        goto done;
    while ((db = list_next (itr))) {
        if (lmt_db_lookup (db, "ost", name) < 0)
            continue; 
        /* FIXME: [schema] no OSS to OST mapping in OST table, so during
         * failover, OST's bandwidth will be attributed to wrong OSS.
         */
        if (lmt_db_insert_ost_data (db, name, read_bytes, write_bytes,
                                    kbytes_free, kbytes_total - kbytes_free,
                                    inodes_free, inodes_total - inodes_free,
                                    errp) < 0) {
            _trigger_db_reconnect ();
            goto done;
        }
        inserts++;
    }
    if (inserts == 0) /* ost not found in any dbs (ESRCH) */
        goto done;
    if (inserts > 1) {
        *errp = "ost is present in more than one db";
        goto done;
    }
done:
    if (itr)
        list_iterator_destroy (itr);
    if (name)
        free (name);
    return retval;
}

int
lmt_db_insert_ost_v2 (char *s, const char **errp)
{
    int retval = -1;
    ListIterator itr = NULL;
    lmt_db_t db;
    char *ostr, *name = NULL;
    float pct_cpu, pct_mem;
    List ostinfo = NULL;
    int inserts = 0;

    if (_init_db_ifneeded (errp, &retval) < 0)
        goto done;
    if (lmt_ost_decode_v2 (s, &name, &pct_cpu, &pct_mem, &ostinfo) < 0) {
        if (errno == EIO)
            *errp = "error parsing ost_v2 string";
        goto done;
    }

    /* Insert the OSS_DATA.
     */
    if (!(itr = list_iterator_create (dbs)))
        goto done;
    while ((db = list_next (itr))) {
        if (lmt_db_lookup (db, "oss", name) < 0)
            continue; 
        if (lmt_db_insert_oss_data (db, name, pct_cpu, pct_mem, errp) < 0) {
            _trigger_db_reconnect ();
            goto done;
        }
        inserts++;
    }
    if (inserts == 0) /* oss not found in any DB's (ESRCH) */
        goto done;
    list_iterator_destroy (itr);

    /* Insert the OST_DATA (for each OST on the OSS).
     */
    if (!(itr = list_iterator_create (ostinfo)))
        goto done;
    while ((ostr = list_next (itr))) {
        if (_insert_ostinfo (ostr, errp) < 0)
            goto done;
    }
    retval = 0;
done:
    if (name)
        free (name);    
    if (itr)
        list_iterator_destroy (itr);        
    if (ostinfo)
        list_destroy (ostinfo);
    return retval;
}

/* helper for _insert_mds () */
static int
_insert_mds_ops (char *mdtname, char *s, const char **errp)
{
    int retval = -1;
    ListIterator itr = NULL;
    lmt_db_t db;
    char *opname = NULL;
    uint64_t samples, sum, sumsquares;
    int inserts = 0;

    if (lmt_mdt_decode_v1_mdops (s, &opname, &samples, &sum, &sumsquares) < 0) {
        if (errno == EIO)
            *errp = "error parsing mdt_v1 string (ops part)";
        goto done;
    }
    if (!(itr = list_iterator_create (dbs)))
        goto done;
    while ((db = list_next (itr))) {
        if (lmt_db_lookup (db, "mdt", mdtname) < 0)
            continue; 
        if (lmt_db_lookup (db, "op", opname) < 0)
            continue; 
        if (lmt_db_insert_mds_ops_data (db, mdtname, opname,
                                        samples, sum, sumsquares, errp) < 0) {
            _trigger_db_reconnect ();
            goto done;
        }
        inserts++;
    }
    if (inserts == 0) /* mdt not found in any DB's (ESRCH) */
        goto done;
    retval = 0;
done:
    if (opname)
        free (opname);    
    if (itr)
        list_iterator_destroy (itr);        
    return retval;
}

/* helper for lmt_db_insert_mdt_v1 () */
static int
_insert_mds (char *mdsname, float pct_cpu, float pct_mem, char *s,
             const char **errp)
{
    int retval = -1;
    ListIterator itr = NULL;
    lmt_db_t db;
    char *op, *mdtname = NULL;
    uint64_t inodes_free, inodes_total;
    uint64_t kbytes_free, kbytes_total;
    List mdops = NULL;
    int inserts = 0;

    if (lmt_mdt_decode_v1_mdtinfo (s, &mdtname, &inodes_free, &inodes_total,
                                   &kbytes_free, &kbytes_total, &mdops) < 0) {
        if (errno == EIO)
            *errp = "error parsing mdt_v1 string (mdt part)";
        goto done;
    }

    /* Insert the MDS_DATA
     * FIXME: [schema] MDS/MDT should be handled like OSS/OST.
     * N.B. To support MDS with MDT's for multiple file systems, we must use
     * mdtname to hash MDS_ID because we will get hits in >1 file system with
     * the mdsname.
     */
    if (!(itr = list_iterator_create (dbs)))
        goto done;
    while ((db = list_next (itr))) {
        if (lmt_db_lookup (db, "mdt", mdtname) < 0)
            continue; 
        /* FIXME: [schema] pct_mem is not used */
        if (lmt_db_insert_mds_data (db, mdtname, pct_cpu,
                                    kbytes_free, kbytes_total - kbytes_free,
                                    inodes_free, inodes_total - inodes_free,
                                                            errp) < 0) {
            _trigger_db_reconnect ();
            goto done;
        }
        inserts++;
    }
    if (inserts == 0) /* mdt not found in any DB's (ESRCH) */
        goto done;
    if (inserts > 1) {
        *errp = "mdt is present in more than one db";
        goto done;
    }
    list_iterator_destroy (itr);

    /* Insert the MDS_OPS_DATA.
     */
    if (!(itr = list_iterator_create (mdops)))
        goto done;
    while ((op = list_next (itr))) {
        if (_insert_mds_ops (mdtname, op, errp) < 0)
            goto done;
    }
    retval = 0;
done:
    if (mdtname)
        free (mdtname);    
    if (itr)
        list_iterator_destroy (itr);        
    if (mdops)
        list_destroy (mdops);
    return retval;
}

int
lmt_db_insert_mdt_v1 (char *s, const char **errp)
{
    int retval = -1;
    ListIterator itr = NULL;
    char *mdt, *mdsname = NULL;
    float pct_cpu, pct_mem;
    List mdtinfo = NULL;

    if (_init_db_ifneeded (errp, &retval) < 0)
        goto done;
    if (lmt_mdt_decode_v1 (s, &mdsname, &pct_cpu, &pct_mem, &mdtinfo) < 0) {
        if (errno == EIO)
            *errp = "error parsing mdt_v1 string (mds part)";
        goto done;
    }
    if (!(itr = list_iterator_create (mdtinfo)))
        goto done;
    while ((mdt = list_next (itr))) {
        if (_insert_mds (mdsname, pct_cpu, pct_mem, mdt, errp) < 0)
            goto done;
    }
    retval = 0;
done:
    if (mdsname)
        free (mdsname);    
    if (itr)
        list_iterator_destroy (itr);        
    if (mdtinfo)
        list_destroy (mdtinfo);
    return retval;
}

int
lmt_db_insert_router_v1 (char *s, const char **errp)
{
    int retval = -1;
    ListIterator itr = NULL;
    lmt_db_t db;
    char *name = NULL;
    float pct_cpu, pct_mem;
    uint64_t bytes;

    if (_init_db_ifneeded (errp, &retval) < 0)
        goto done;
    if (lmt_router_decode_v1 (s, &name, &pct_cpu, &pct_mem, &bytes) < 0) {
        if (errno == EIO)
            *errp = "error parsing router_v1 string";
        goto done;
    }
    if (!(itr = list_iterator_create (dbs)))
        goto done;
    /* FIXME: [schema] pct_mem is not recorded */
    while ((db = list_next (itr))) {
        if (lmt_db_lookup (db, "router", name) < 0)
            goto done; /* router not present in all db's (ESRCH) */
        if (lmt_db_insert_router_data (db, name, bytes, pct_cpu, errp) < 0) {
            _trigger_db_reconnect ();
            goto done;
        }
    }
    retval = 0;
done:
    if (name)
        free (name);
    if (itr)
        list_iterator_destroy (itr);        
    return retval;
}

/**
 ** Legacy
 **/

int
lmt_db_insert_mds_v2 (char *s, const char **errp)
{
    int retval = -1;
    ListIterator itr = NULL;
    lmt_db_t db;
    char *op, *mdtname = NULL, *mdsname = NULL;
    float pct_cpu, pct_mem;
    uint64_t inodes_free, inodes_total;
    uint64_t kbytes_free, kbytes_total;
    List mdops = NULL;
    int inserts = 0;

    if (_init_db_ifneeded (errp, &retval) < 0)
        goto done;
    if (lmt_mds_decode_v2 (s, &mdsname, &mdtname, &pct_cpu, &pct_mem,
                           &inodes_free, &inodes_total,
                           &kbytes_free, &kbytes_total, &mdops) < 0) {
        if (errno == EIO)
            *errp = "error parsing mds_v2 string";
        goto done;
    }
    if (!(itr = list_iterator_create (dbs)))
        goto done;
    while ((db = list_next (itr))) {
        if (lmt_db_lookup (db, "mdt", mdtname) < 0)
            continue; 
        if (lmt_db_insert_mds_data (db, mdtname, pct_cpu,
                                    kbytes_free, kbytes_total - kbytes_free,
                                    inodes_free, inodes_total - inodes_free,
                                                            errp) < 0) {
            _trigger_db_reconnect ();
            goto done;
        }
        inserts++;
    }
    if (inserts == 0) /* mds not found in any DB's (ESRCH) */
        goto done;
    if (inserts > 1) {
        *errp = "mdt is present in more than one db";
        goto done;
    }
    list_iterator_destroy (itr);

    if (!(itr = list_iterator_create (mdops)))
        goto done;
    while ((op = list_next (itr))) {
        if (_insert_mds_ops (mdtname, op, errp) < 0)
            goto done;
    }
    retval = 0;
done:
    if (mdtname)
        free (mdtname);    
    if (mdsname)
        free (mdsname);    
    if (mdops)
        list_destroy (mdops);
    if (itr)
        list_iterator_destroy (itr);        
    return retval;
}

int
lmt_db_insert_oss_v1 (char *s, const char **errp)
{
    int retval = -1;
    ListIterator itr = NULL;
    lmt_db_t db;
    char *name = NULL;
    float pct_cpu, pct_mem;
    int inserts = 0;

    if (_init_db_ifneeded (errp, &retval) < 0)
        goto done;
    if (lmt_oss_decode_v1 (s, &name, &pct_cpu, &pct_mem) < 0) {
        if (errno == EIO)
            *errp = "error parsing oss_v1 string";
        goto done;
    }
    if (!(itr = list_iterator_create (dbs)))
        goto done;
    while ((db = list_next (itr))) {
        if (lmt_db_lookup (db, "oss", name) < 0)
            continue; 
        if (lmt_db_insert_oss_data (db, name, pct_cpu, pct_mem, errp) < 0) {
            _trigger_db_reconnect ();
            goto done;
        }
        inserts++;
    }
    if (inserts == 0) /* oss not found in any DB's (ESRCH) */
        goto done;
    retval = 0;
done:
    if (name)
        free (name);    
    if (itr)
        list_iterator_destroy (itr);        
    return retval;
}

int
lmt_db_insert_ost_v1 (char *s, const char **errp)
{
    int retval = -1;
    ListIterator itr = NULL;
    lmt_db_t db;
    char *ossname = NULL, *name = NULL;
    uint64_t read_bytes, write_bytes;
    uint64_t kbytes_free, kbytes_total;
    uint64_t inodes_free, inodes_total;
    int inserts = 0;

    if (_init_db_ifneeded (errp, &retval) < 0)
        goto done;

    if (lmt_ost_decode_v1 (s, &ossname, &name, &read_bytes, &write_bytes,
                           &kbytes_free, &kbytes_total,
                           &inodes_free, &inodes_total) < 0) {
        if (errno == EIO)
            *errp = "error parsing ost_v1 string";
        goto done;
    }
    if (!(itr = list_iterator_create (dbs)))
        goto done;
    while ((db = list_next (itr))) {
        if (lmt_db_lookup (db, "ost", name) < 0)
            continue; 
        if (lmt_db_insert_ost_data (db, name, read_bytes, write_bytes,
                                    kbytes_free, kbytes_total - kbytes_free,
                                    inodes_free, inodes_total - inodes_free,
                                    errp) < 0) {
            _trigger_db_reconnect ();
            goto done;
        }
        inserts++;
    }
    if (inserts == 0) /* oss not found in any DB's (ESRCH) */
        goto done;
    retval = 0;
done:
    if (name)
        free (name);    
    if (itr)
        list_iterator_destroy (itr);        
    return retval;
}

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
