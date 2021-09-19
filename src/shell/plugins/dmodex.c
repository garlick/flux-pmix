/************************************************************\
 * Copyright 2021 Lawrence Livermore National Security, LLC
 * (c.f. AUTHORS, NOTICE.LLNS, COPYING)
 *
 * This file is part of the Flux resource manager framework.
 * For details, see https://github.com/flux-framework.
 *
 * SPDX-License-Identifier: LGPL-3.0
\************************************************************/

/* dmodex.c - handle direct_modex callback from openpmix server
 *
 * The direct_modex callback is essentially requesting a remote procedure call
 * of PMIx_server_dmodex_request() on another shell.  Its purpose is to
 * service PMIx_Get() requests where the proc.rank argument specifies a
 * non-local rank, and the requested key has not already been assigned a
 * value locally, e.g. via a collective exchange.
 *
 * N.B. openpmix callbacks are made in the context of the pmix server thread.
 * For flux, these are converted to interthread messages so the callbacks can
 * wake the shell's reactor loop and pass information without locks.  In this
 * module there are two such callbacks: the initial direct_modex callback,
 * and the user callback passed in to PMIx_server_dmodex_request().
 *
 * (local)                                   (remote)
 * Server Thread                                 Server Thread
 *     Shell Thread                          Shell Thread
 *
 * dmodex_server_cb()
 *     |
 *     | (interthread)
 *     v                    (pmix-dmodex RPC)
 *     dmodex_shell_cb() ------------------> dmodex_rpc_handler()
 *                                           PMIx_server_dmodex_request()
 *                                               dmodex_request_server_cb()
 *                                               |
 *                                               | (interthread)
 *                              (RPC response)   v
 *     dmodex_rpc_continuation() <---------- dmodex_request_shell_cb()
 *     cbfunc()
 */

#if HAVE_CONFIG_H
#include "config.h"
#endif
#include <jansson.h>
#include <flux/core.h>
#include <flux/shell.h>
#include <pmix.h>
#include <pmix_server.h>

#include "codec.h"
#include "interthread.h"

#include "dmodex.h"

struct dmodex {
    flux_shell_t *shell;
    struct interthread *it;
};

/* Context for a single direct_modex callback (local shell only).
 */
struct dmodex_call {
    pmix_proc_t proc;
    int shell_rank;
    pmix_modex_cbfunc_t cbfunc;
    void *cbdata;
};

/* Context for PMix_server_dmodex_request() 'cbdata' (remote shell only)
 */
struct dmodex_request {
    const flux_msg_t *msg;
    flux_t *h;
    pmix_proc_t proc;
};

/* This is for the benefit of server callbacks that don't have
 * a way to be passed a user-supplied opaque pointer.
 */
static struct dmodex *global_dmodex_ctx;

/* Tracing is always on for now.
 */
static bool dmodex_trace_flag = true;

static void dmodex_call_destroy (struct dmodex_call *dxcall)
{
    if (dxcall) {
        int saved_errno = errno;
        free (dxcall);
        errno = saved_errno;
    };
}

static struct dmodex_call *dmodex_call_create (void)
{
    struct dmodex_call *dxcall;

    if (!(dxcall = calloc (1, sizeof (*dxcall))))
        return NULL;
    return dxcall;
}

static void dmodex_request_destroy (struct dmodex_request *dxreq)
{
    if (dxreq) {
        int saved_errno = errno;
        flux_msg_decref (dxreq->msg);
        free (dxreq);
        errno = saved_errno;
    };
}

static struct dmodex_request *dmodex_request_create (flux_t *h,
                                                     const flux_msg_t *msg)
{
    struct dmodex_request *dxreq;

    if (!(dxreq = calloc (1, sizeof (*dxreq))))
        return NULL;
    dxreq->h = h;
    dxreq->msg = flux_msg_incref (msg);
    return dxreq;
}

/* Find the shell rank that hosts proc 'rank', or return -1 if not found.
 */
static int lookup_shell_rank (flux_shell_t *shell, int rank)
{
    int shell_size;
    int shell_rank;
    int base_rank = 0;

    if (flux_shell_info_unpack (shell, "{s:i}", "size", &shell_size) < 0)
        goto error;
    for (shell_rank = 0; shell_rank < shell_size; shell_rank++) {
        int ntasks;
        if (flux_shell_rank_info_unpack (shell,
                                         shell_rank,
                                         "{s:i}",
                                         "ntasks", &ntasks) < 0)
            goto error;
        if (rank >= base_rank && rank < base_rank + ntasks)
            return shell_rank;
        base_rank += ntasks;
    }
error:
    errno = ENOENT;
    return -1;
}

/* Decode RPC response, then call the cbfunc() the server passed to us
 * with the original direct_modex() callback to hand over the data
 * and status from the remote PMIx_server_dmodex_request().
 * All done with this direct_modex callback.
 */
static void dmodex_rpc_continuation (flux_future_t *f, void *arg)
{
    struct dmodex *dx = arg;
    struct dmodex_call *dxcall = flux_future_aux_get (f, "dxcall");
    int status;
    json_t *xdata;
    size_t ndata = 0;
    void *data = NULL;

    if (flux_rpc_get_unpack (f,
                             "{s:i s:o}",
                             "status", &status,
                             "data", &xdata) < 0
        || codec_data_decode (xdata, &data, &ndata) < 0) {
        shell_warn ("error decoding dmodex RPC response");
        status = PMIX_ERROR;
    }
    if (dmodex_trace_flag) {
        shell_trace ("pmix-dmodex response for %s.%d from shell rank %d: %s",
                     dxcall->proc.nspace,
                     dxcall->proc.rank,
                     dxcall->shell_rank,
                     PMIx_Error_string (status));
    }
    if (dxcall->cbfunc)
        dxcall->cbfunc (status, data, ndata, dxcall->cbdata, free, data);
    else
        free (data);
    flux_future_destroy (f);
}

/* Decode the interthread message to get status and data from the local pmix
 * server, then respond to the pmix-dmodex RPC request message that was passed
 * into PMIx_server_dmodex_request() as 'cbdata'.
 */
static void dmodex_request_shell_cb (const flux_msg_t *msg, void *arg)
{
    struct dmodex *dx = arg;
    json_t *xdata;
    json_t *xcbdata;
    int status;
    void *data = NULL;
    size_t size;
    void *cbdata;
    struct dmodex_request *dxreq;

    if (flux_msg_unpack (msg,
                         "{s:i s:o s:o}",
                         "status", &status,
                         "data", &xdata,
                         "cbdata", &xcbdata) < 0
        || codec_data_decode (xdata, &data, &size) < 0
        || codec_pointer_decode (xcbdata, &cbdata)) {
        shell_warn ("error unpacking dmodex_request interthread message");
        return;
    }
    dxreq = cbdata;
    if (flux_respond_pack (dxreq->h,
                           dxreq->msg,
                          "{s:i s:O}",
                          "status", status,
                          "data", xdata) < 0) {
        shell_warn ("error responding to pmix-dmodex RPC");
        goto done;
    }
    if (dmodex_trace_flag) {
        shell_trace ("pmix-dmodex response for %s.%d sent",
                     dxreq->proc.nspace,
                     dxreq->proc.rank);
    }
done:
    dmodex_request_destroy (dxreq);
    free (data);
}

/* This user PMIx_server_dmodex_request() callback encodes its parameters as
 * json and sends them as an interthread message to dmodex_request_shell_cb().
 * N.B. server thread context here - DO NOT call shell API.
 */
static void dmodex_request_server_cb (pmix_status_t status,
                                      char *data,
                                      size_t size,
                                      void *cbdata)
{
    struct dmodex *dx = global_dmodex_ctx;
    json_t *xdata = NULL;
    json_t *xcbdata = NULL;

    if (!(xdata = codec_data_encode (data, size))
        || !(xcbdata = codec_pointer_encode (cbdata))
        || interthread_send_pack (dx->it,
                                  "dmodex_request",
                                  "{s:i s:O s:O}",
                                  "status", status,
                                  "data", xdata,
                                  "cbdata", xcbdata) < 0) {
        fprintf (stderr,
                 "error sending dmodex_request interthread message\n");
    }
    json_decref (xdata);
    json_decref (xcbdata);
}

/* Call PMIx_server_dmodex_request() on behalf of remote shell.
 * When the server completes the request, it calls dmodex_request_server_cb().
 */
static void dmodex_rpc_handler (flux_t *h,
                                flux_msg_handler_t *mh,
                                const flux_msg_t *msg,
                                void *arg)
{
    struct dmodex *dx = arg;
    json_t *xproc;
    json_t *xinfo;
    const char *errmsg = NULL;
    struct dmodex_request *dxreq = NULL;
    int rc;

    if (!(dxreq = dmodex_request_create (h, msg))
        || flux_request_unpack (msg,
                             NULL,
                             "{s:o s:o}",
                             "proc", &xproc,
                             "info", &xinfo) < 0
        || codec_proc_decode (xproc, &dxreq->proc) < 0) {
        errno = EPROTO;
        goto error;
    }
    /* FIXME process info[] options:
     * pmix.req.key (bool) - true=delay response until key available/timeout
     * pmix.timeout (int) - wait <val> seconds for key, then PMIX_ERR_TIMEOUT
     */
    if ((rc = PMIx_server_dmodex_request (&dxreq->proc,
                                          dmodex_request_server_cb,
                                          dxreq)) != PMIX_SUCCESS) {
        errno = EINVAL;
        errmsg = PMIx_Error_string (rc);
        goto error;
    }
    if (dmodex_trace_flag) {
        shell_trace ("pmix-dmodex request invoked for %s.%d",
                     dxreq->proc.nspace,
                     dxreq->proc.rank);
    };
    return;
error:
    if (flux_respond_error (h, msg, errno, errmsg) < 0)
        shell_warn ("error responding to pmix-dmodex RPC");
    dmodex_request_destroy (dxreq);
}

/* Determine the remote shell hosting proc.rank, then forward 'proc'
 * and 'info' to that shell using the pmix-dmodex RPC.
 * dmodex_rpc_handler() receives the RPC message in the remote shell.
 * dmodex_rpc_continuation() handles the RPC response in this shell.
 */
static void dmodex_shell_cb (const flux_msg_t *msg, void *arg)
{
    struct dmodex *dx = arg;
    json_t *xproc;
    json_t *xinfo;
    json_t *xcbfunc;
    json_t *xcbdata;
    struct dmodex_call *dxcall;
    int rc;
    flux_future_t *f;

    if (!(dxcall = dmodex_call_create ())
        || flux_msg_unpack (msg,
                            "{s:o s:o s:o s:o}",
                            "proc", &xproc,
                            "info", &xinfo,
                            "cbfunc", &xcbfunc,
                            "cbdata", &xcbdata) < 0
        || codec_proc_decode (xproc, &dxcall->proc) < 0
        || codec_pointer_decode (xcbfunc, (void **)&dxcall->cbfunc) < 0
        || codec_pointer_decode (xcbdata, &dxcall->cbdata) < 0) {
        shell_warn ("error unpacking dmodex_upcall interthread message");
        dmodex_call_destroy (dxcall);
        return;
    }
    if ((dxcall->shell_rank = lookup_shell_rank (dx->shell,
                                                 dxcall->proc.rank)) < 0) {
        shell_warn ("could not find dmodex target rank %d", dxcall->proc.rank);
        rc = PMIX_ERR_PROC_ENTRY_NOT_FOUND;
        goto error;
    }
    if (!(f = flux_shell_rpc_pack (dx->shell,
                                   "pmix-dmodex",
                                   dxcall->shell_rank,
                                   0,
                                   "{s:o s:o}",
                                   "proc", xproc,
                                   "info", xinfo))
        || flux_future_then (f, -1, dmodex_rpc_continuation, dx) < 0
        || flux_future_aux_set (f,
                                "dxcall",
                                dxcall,
                                (flux_free_f)dmodex_call_destroy) < 0) {
        flux_future_destroy (f);
        rc = PMIX_ERROR;
        shell_warn ("error sending pmix-dmodex request to shell rank %d",
                    dxcall->shell_rank);
        goto error;
    }
    if (dmodex_trace_flag) {
        shell_trace ("pmix-dmodex request for %s.%d sent to shell rank %d",
                     dxcall->proc.nspace,
                     dxcall->proc.rank,
                     dxcall->shell_rank);
    }
    return;
error:
    if (dxcall->cbfunc)
        dxcall->cbfunc (rc, NULL, 0, dxcall->cbdata, NULL, NULL);
    dmodex_call_destroy (dxcall);
}

/* The initial direct_modex callback encodes its parameters as json
 * and sends them as an interthread message to dmodex_shell_cb().
 * N.B. server thread context here - DO NOT call shell API.
 */
int dmodex_server_cb (const pmix_proc_t *proc,
                      const pmix_info_t info[],
                      size_t ninfo,
                      pmix_modex_cbfunc_t cbfunc,
                      void *cbdata)
{
    struct dmodex *dx = global_dmodex_ctx;
    json_t *xproc  = NULL;
    json_t *xinfo = NULL;
    json_t *xcbfunc = NULL;
    json_t *xcbdata = NULL;
    int rc = PMIX_SUCCESS;

    if (!(xproc = codec_proc_encode (proc))
        || !(xinfo = codec_info_array_encode (info, ninfo))
        || !(xcbfunc = codec_pointer_encode (cbfunc))
        || !(xcbdata = codec_pointer_encode (cbdata))
        || interthread_send_pack (dx->it,
                                  "dmodex_upcall",
                                  "{s:O s:O s:O s:O}",
                                  "proc", xproc,
                                  "info", xinfo,
                                  "cbfunc", xcbfunc,
                                  "cbdata", xcbdata) < 0) {
        fprintf (stderr, "error sending dmodex_upcall interthread message\n");
        rc = PMIX_ERROR;
    }
    json_decref (xproc);
    json_decref (xinfo);
    json_decref (xcbfunc);
    json_decref (xcbdata);
    return rc;
}

void dmodex_destroy (struct dmodex *dx)
{
    if (dx) {
        int saved_errno = errno;
        free (dx);
        errno = saved_errno;
        global_dmodex_ctx = NULL;
    }
}

struct dmodex *dmodex_create (flux_shell_t *shell, struct interthread *it)
{
    flux_t *h = flux_shell_get_flux (shell);
    struct dmodex *dx;

    if (!(dx = calloc (1, sizeof (*dx))))
        return NULL;
    dx->shell = shell;
    dx->it = it;
    if (interthread_register (it,
                              "dmodex_upcall",
                              dmodex_shell_cb,
                              dx) < 0
        || interthread_register (it,
                                 "dmodex_request",
                                 dmodex_request_shell_cb,
                                 dx))
        goto error;
    if (flux_shell_service_register (shell,
                                     "pmix-dmodex",
                                     dmodex_rpc_handler,
                                     dx) < 0)
        goto error;
    global_dmodex_ctx = dx;
    return dx;
error:
    dmodex_destroy (dx);
    return NULL;
}

// vi:ts=4 sw=4 expandtab
