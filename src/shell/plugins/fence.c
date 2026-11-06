/************************************************************\
 * Copyright 2021 Lawrence Livermore National Security, LLC
 * (c.f. AUTHORS, NOTICE.LLNS, COPYING)
 *
 * This file is part of the Flux resource manager framework.
 * For details, see https://github.com/flux-framework.
 *
 * SPDX-License-Identifier: LGPL-3.0
\************************************************************/

/* fence.c - handle fence_nb callback from openpmix server
 *
 * A fence is an exchange on a jobdict.  The jobdict shell plugin in
 * flux-core provides the jobdict and the exchange.  This plugin uses
 * them through the plugin stack.  See flux-shell-plugins(7).
 *
 * On each fence, the shell puts the blob from openpmix under a key
 * named for its rank.  Then it enters an exchange among the shells
 * that host the fence procs.  The result has one blob from each of
 * those shells.  The blobs are concatenated for the fence_nb callback,
 * in any order.
 *
 * openpmix passes a fence over explicit procs to the host when all
 * local procs have entered it.  The procs are mapped to the shells
 * that host them.  That set of shells is the exchange participants.
 * The exchange tag is the set of shells and a sequence number for
 * that set.  Clients may list the procs in any order, but the set is
 * the same, so the tags agree on all shells.
 *
 * The shell puts a blob on each fence.  If the fence does not collect
 * data, the blob is empty.  A jobdict carries a key put before a
 * subset exchange forward to the next exchange among all shells.  The
 * empty blob prevents an old blob from being exchanged again.
 */

#if HAVE_CONFIG_H
#include "config.h"
#endif
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <jansson.h>
#include <flux/core.h>
#include <flux/shell.h>
#include <flux/idset.h>
#include <flux/taskmap.h>
#include <pmix.h>
#include <pmix_server.h>

#include "codec.h"
#include "interthread.h"

#include "fence.h"

#define FENCE_JOBDICT "pmix"
#define FENCE_EXIT_TOPIC "pmix.fence-exit"

struct fence_call {
    pmix_proc_t *procs;
    size_t nprocs;
    pmix_info_t *info;
    size_t ninfo;
    pmix_modex_cbfunc_t cbfunc;
    void *cbdata;
    bool collect;
    int exchange_seq;
    char *tag;
    struct fence_call *next;    // next fence in progress
};

/* The sequence number of the next exchange over a set of shells.
 */
struct fence_seq {
    char *set;                  // encoded idset of shells, or "all"
    int seq;
    struct fence_seq *next;
};

struct fence {
    flux_shell_t *shell;
    struct interthread *it;
    const struct taskmap *taskmap;
    int shell_rank;
    int shell_size;
    char key[32];               // this shell's jobdict key
    int trace_flag;
    int exchange_seq;
    struct fence_seq *seqs;     // one per set of shells seen
    struct fence_call *calls;   // fences in progress, found by tag
};

/* Server callbacks have no argument for an opaque pointer.
 * They find the context here.
 */
static struct fence *global_fence_ctx;

static void fence_call_destroy (struct fence_call *fxcall)
{
    if (fxcall) {
        int saved_errno = errno;
        free (fxcall->procs);
        codec_info_array_destroy (fxcall->info, fxcall->ninfo);
        free (fxcall->tag);
        free (fxcall);
        errno = saved_errno;
    }
}

static struct fence_call *fence_call_create (struct fence *fx)
{
    struct fence_call *fxcall;

    if (!(fxcall = calloc (1, sizeof (*fxcall))))
        return NULL;
    fxcall->exchange_seq = fx->exchange_seq++;
    return fxcall;
}

/* Add a fence to the list of fences in progress.
 */
static void calls_push (struct fence *fx, struct fence_call *fxcall)
{
    fxcall->next = fx->calls;
    fx->calls = fxcall;
}

/* Remove the fence with this tag from the list of fences in progress.
 * Return the fence, or NULL if there is no fence with this tag.
 */
static struct fence_call *calls_pop (struct fence *fx, const char *tag)
{
    struct fence_call **pp;

    for (pp = &fx->calls; *pp != NULL; pp = &(*pp)->next) {
        if (!strcmp ((*pp)->tag, tag)) {
            struct fence_call *fxcall = *pp;
            *pp = fxcall->next;
            fxcall->next = NULL;
            return fxcall;
        }
    }
    return NULL;
}

/* Call a jobdict topic through the plugin stack.
 * On success, return 0.  If argsp is set, the caller gets the args and
 * must destroy them.  On failure, log the errmsg from jobdict and
 * return -1.
 */
static int jobdict_call (struct fence *fx,
                         const char *topic,
                         flux_plugin_arg_t **argsp,
                         const char *fmt,
                         ...)
{
    flux_plugin_arg_t *args;
    const char *errmsg = NULL;
    va_list ap;
    int rc;

    if (!(args = flux_plugin_arg_create ())) {
        shell_warn ("%s: error creating args", topic);
        return -1;
    }
    va_start (ap, fmt);
    rc = flux_plugin_arg_vpack (args, FLUX_PLUGIN_ARG_IN, fmt, ap);
    va_end (ap);
    if (rc < 0) {
        shell_warn ("%s: error packing args", topic);
        goto error;
    }
    if (flux_shell_plugstack_call (fx->shell, topic, args) < 0) {
        (void)flux_plugin_arg_unpack (args,
                                      FLUX_PLUGIN_ARG_OUT,
                                      "{s:s}",
                                      "errmsg", &errmsg);
        shell_warn ("%s: %s", topic, errmsg ? errmsg : "failed");
        goto error;
    }
    if (argsp)
        *argsp = args;
    else
        flux_plugin_arg_destroy (args);
    return 0;
error:
    flux_plugin_arg_destroy (args);
    return -1;
}

/* Concatenate the blobs in result into one buffer.  The caller must
 * free the buffer.  The order of the blobs is not specified.  The
 * fence_nb callback permits any order.
 */
static int result_to_data (json_t *result, void **datap, size_t *sizep)
{
    size_t bufsize = 0;
    const char *key;
    json_t *value;
    uint8_t *data = NULL;
    size_t size = 0;

    json_object_foreach (result, key, value) {
        ssize_t chunksize = codec_data_decode_bufsize (value);
        if (chunksize < 0)
            return -1;
        bufsize += chunksize;
    }
    if (!(data = malloc (bufsize > 0 ? bufsize : 1)))
        return -1;
    json_object_foreach (result, key, value) {
        ssize_t chunksize = codec_data_decode_tobuf (value,
                                                     data + size,
                                                     bufsize - size);
        if (chunksize < 0)
            goto error;
        size += chunksize;
    }
    *datap = data;
    *sizep = size;
    return 0;
error:
    free (data);
    return -1;
}

/* The jobdict exchange is complete on this shell.  It may have failed.
 */
static int exchange_exit_cb (flux_plugin_t *p,
                             const char *topic,
                             flux_plugin_arg_t *args,
                             void *arg)
{
    struct fence *fx = arg;
    const char *tag;
    const char *errmsg = NULL;
    json_t *result = NULL;
    struct fence_call *fxcall;
    void *data = NULL;
    size_t ndata = 0;
    int status = PMIX_ERROR;

    if (flux_plugin_arg_unpack (args,
                                FLUX_PLUGIN_ARG_IN,
                                "{s:s s?s s?o}",
                                "tag", &tag,
                                "errmsg", &errmsg,
                                "result", &result) < 0) {
        shell_warn ("error unpacking %s args", topic);
        return -1;
    }
    if (!(fxcall = calls_pop (fx, tag))) {
        shell_warn ("%s: no fence in progress for %s", topic, tag);
        return -1;
    }
    if (errmsg) {
        shell_warn ("pmix exchange %d failed: %s",
                    fxcall->exchange_seq,
                    errmsg);
        goto done;
    }
    if (!result || result_to_data (result, &data, &ndata) < 0) {
        shell_warn ("error accessing pmix exchanged data");
        goto done;
    }
    status = PMIX_SUCCESS;
done:
    shell_trace ("completed pmix exchange %d (%s): size %zu %s",
                 fxcall->exchange_seq,
                 fxcall->tag,
                 ndata,
                 PMIx_Error_string (status));
    // openpmix frees data when the fence is complete
    fxcall->cbfunc (status, data, ndata, fxcall->cbdata, free, data);
    fence_call_destroy (fxcall);
    return 0;
}

/* Parse one attribute from the fence info.
 * Return PMIX_SUCCESS or an error status.
 */
static int parse_fence_attr (struct fence_call *fxcall, pmix_info_t *info)
{
    if (!strcmp (info->key, "pmix.collect")) {
        if (info->value.type != PMIX_BOOL) {
            shell_warn ("fence attr %s has wrong type=%d",
                        info->key,
                        info->value.type);
            return PMIX_ERR_BAD_PARAM;
        }
        if (info->value.data.flag == true)
            fxcall->collect = true;
    }
    else if ((info->flags & PMIX_INFO_REQD)) {
        shell_warn ("unknown required fence attr: %s", info->key);
        return PMIX_ERR_BAD_PARAM;
    }
    else {
        shell_debug ("ignoring unknown optional fence attr: %s", info->key);
    }
    return 0;
}

/* Return the next sequence number for exchanges over set.
 * Return -1 on failure.
 */
static int seq_next (struct fence *fx, const char *set)
{
    struct fence_seq *fs;

    for (fs = fx->seqs; fs != NULL; fs = fs->next) {
        if (!strcmp (fs->set, set))
            return fs->seq++;
    }
    if (!(fs = calloc (1, sizeof (*fs))) || !(fs->set = strdup (set))) {
        free (fs);
        return -1;
    }
    fs->next = fx->seqs;
    fx->seqs = fs;
    return fs->seq++;
}

/* Map the fence procs to the shells that host them.
 * Return the set of shells as a new idset.  Return NULL if the set is
 * all shells, or on error.  Set *statusp to PMIX_SUCCESS or an error.
 * A fence over one wildcard proc is over all shells.
 */
static struct idset *participants_create (struct fence *fx,
                                          struct fence_call *fxcall,
                                          int *statusp)
{
    struct idset *ids = NULL;
    int status = PMIX_ERR_NOT_SUPPORTED;

    if (fxcall->nprocs == 0) {
        status = PMIX_ERR_BAD_PARAM;
        goto error;
    }
    if (!(ids = idset_create (0, IDSET_FLAG_AUTOGROW))) {
        status = PMIX_ERR_NOMEM;
        goto error;
    }
    for (size_t i = 0; i < fxcall->nprocs; i++) {
        const pmix_proc_t *proc = &fxcall->procs[i];
        int nodeid;

        if (strcmp (proc->nspace, fxcall->procs[0].nspace) != 0) {
            shell_warn ("fence over multiple namespaces is not supported");
            goto error;
        }
        if (proc->rank == PMIX_RANK_WILDCARD) {
            if (fxcall->nprocs != 1) {
                shell_warn ("fence mixes wildcard and explicit ranks");
                goto error;
            }
            idset_destroy (ids);
            *statusp = PMIX_SUCCESS;
            return NULL;
        }
        if ((nodeid = taskmap_nodeid (fx->taskmap, proc->rank)) < 0) {
            shell_warn ("fence names rank %u, which is not in the job",
                        (unsigned int)proc->rank);
            status = PMIX_ERR_BAD_PARAM;
            goto error;
        }
        if (idset_set (ids, nodeid) < 0) {
            status = PMIX_ERR_NOMEM;
            goto error;
        }
    }
    if (!idset_test (ids, fx->shell_rank)) {
        shell_warn ("fence over procs that are not local reached the host");
        goto error;
    }
    if (idset_count (ids) == fx->shell_size) {
        idset_destroy (ids);
        ids = NULL;
    }
    *statusp = PMIX_SUCCESS;
    return ids;
error:
    idset_destroy (ids);
    *statusp = status;
    return NULL;
}

/* Create the exchange tag for a fence over ids, or over all shells if
 * ids is NULL.  The tag is the set of shells and a sequence number
 * for that set.  Return NULL on failure.
 */
static char *tag_create (struct fence *fx, const struct idset *ids)
{
    char *set = NULL;
    char *tag = NULL;
    int seq;

    if (ids) {
        if (!(set = idset_encode (ids, IDSET_FLAG_RANGE)))
            return NULL;
    }
    else if (!(set = strdup ("all")))
        return NULL;
    if ((seq = seq_next (fx, set)) < 0
        || asprintf (&tag, "%s:%d", set, seq) < 0)
        tag = NULL;
    free (set);
    return tag;
}

static void fence_shell_cb (const flux_msg_t *msg, void *arg)
{
    struct fence *fx = arg;
    json_t *xprocs;
    json_t *xinfo;
    json_t *xdata;
    json_t *xcbfunc;
    json_t *xcbdata;
    json_t *xempty = NULL;
    struct fence_call *fxcall;
    struct idset *participants = NULL;
    char *participants_str = NULL;
    int rc;

    if (!(fxcall = fence_call_create (fx))
        || flux_msg_unpack (msg,
                            "{s:o s:o s:o s:o s:o}",
                            "procs", &xprocs,
                            "info", &xinfo,
                            "data", &xdata, // not further decoded here
                            "cbfunc", &xcbfunc,
                            "cbdata", &xcbdata) < 0
        || codec_proc_array_decode (xprocs,
                                    &fxcall->procs,
                                    &fxcall->nprocs) < 0
        || codec_info_array_decode (xinfo,
                                    &fxcall->info,
                                    &fxcall->ninfo) < 0
        || codec_pointer_decode (xcbfunc, (void **)&fxcall->cbfunc) < 0
        || codec_pointer_decode (xcbdata, &fxcall->cbdata) < 0
        || fxcall->cbfunc == NULL) {
        shell_warn ("error unpacking interthread fence_upcall message");
        fence_call_destroy (fxcall);
        return;
    }
    for (int i = 0; i < fxcall->ninfo; i++) {
        rc = parse_fence_attr (fxcall, &fxcall->info[i]);
        if (rc != PMIX_SUCCESS)
            goto error;
    }
    participants = participants_create (fx, fxcall, &rc);
    if (rc != PMIX_SUCCESS)
        goto error;
    if (participants) {
        participants_str = idset_encode (participants, IDSET_FLAG_RANGE);
        if (!participants_str) {
            shell_warn ("error encoding fence participants");
            rc = PMIX_ERR_NOMEM;
            goto error;
        }
    }
    if (!(xempty = codec_data_encode (NULL, 0))) {
        shell_warn ("error encoding empty fence data");
        rc = PMIX_ERR_NOMEM;
        goto error;
    }
    if (!(fxcall->tag = tag_create (fx, participants))) {
        shell_warn ("error creating fence exchange tag");
        rc = PMIX_ERR_NOMEM;
        goto error;
    }
    if (fx->trace_flag) {
        shell_trace ("starting pmix exchange %d (%s): size %zi",
                     fxcall->exchange_seq,
                     fxcall->tag,
                     fxcall->collect ? codec_data_length (xdata) : 0);
    }
    /* Put the blob.  If the fence does not collect data, put an empty
     * blob instead.
     */
    if (jobdict_call (fx,
                      "jobdict.put",
                      NULL,
                      "{s:s s:s s:O}",
                      "name", FENCE_JOBDICT,
                      "key", fx->key,
                      "value", fxcall->collect ? xdata : xempty) < 0) {
        rc = PMIX_ERROR;
        goto error;
    }
    /* The exchange can complete before jobdict.exchange returns.
     * The fence must be in the list before the exit callback runs.
     */
    calls_push (fx, fxcall);
    if (jobdict_call (fx,
                      "jobdict.exchange",
                      NULL,
                      "{s:s s:s s:s? s:s}",
                      "name", FENCE_JOBDICT,
                      "tag", fxcall->tag,
                      "participants", participants_str,
                      "exit_topic", FENCE_EXIT_TOPIC) < 0) {
        (void)calls_pop (fx, fxcall->tag);
        rc = PMIX_ERROR;
        goto error;
    }
    json_decref (xempty);
    free (participants_str);
    idset_destroy (participants);
    return;
error:
    json_decref (xempty);
    free (participants_str);
    idset_destroy (participants);
    fxcall->cbfunc (rc, NULL, 0, fxcall->cbdata, NULL, NULL);
    fence_call_destroy (fxcall);
}

int fence_server_cb (const pmix_proc_t procs[],
                     size_t nprocs,
                     const pmix_info_t info[],
                     size_t ninfo,
                     char *data,
                     size_t ndata,
                     pmix_modex_cbfunc_t cbfunc,
                     void *cbdata)
{
    struct fence *fx = global_fence_ctx;
    json_t *xprocs = NULL;
    json_t *xinfo = NULL;
    json_t *xdata = NULL;
    json_t *xcbfunc = NULL;
    json_t *xcbdata = NULL;
    int rc = PMIX_SUCCESS;

    if (!(xprocs = codec_proc_array_encode (procs, nprocs))
        || !(xinfo = codec_info_array_encode (info, ninfo))
        || !(xdata = codec_data_encode (data, ndata))
        || !(xcbfunc = codec_pointer_encode (cbfunc))
        || !(xcbdata = codec_pointer_encode (cbdata))
        || interthread_send_pack (fx->it,
                                  "fence_upcall",
                                  "{s:O s:O s:O s:O s:O}",
                                  "procs", xprocs,
                                  "info", xinfo,
                                  "data", xdata,
                                  "cbfunc", xcbfunc,
                                  "cbdata", xcbdata) < 0) {
        fprintf (stderr, "error sending fence_upcall interthread message\n");
        rc = PMIX_ERROR;
    }
    json_decref (xprocs);
    json_decref (xinfo);
    json_decref (xdata);
    json_decref (xcbfunc);
    json_decref (xcbdata);
    /* openpmix gives the host ownership of data.  There is no release
     * function.  The data is now in xdata, so free it here.
     */
    free (data);
    return rc;
}

void fence_destroy (struct fence *fx)
{
    if (fx) {
        int saved_errno = errno;
        while (fx->calls) {
            struct fence_call *fxcall = fx->calls;
            fx->calls = fxcall->next;
            fence_call_destroy (fxcall);
        }
        while (fx->seqs) {
            struct fence_seq *fs = fx->seqs;
            fx->seqs = fs->next;
            free (fs->set);
            free (fs);
        }
        free (fx);
        errno = saved_errno;
        global_fence_ctx = NULL;
    }
}

struct fence *fence_create (flux_plugin_t *p,
                            struct interthread *it,
                            int k)
{
    flux_shell_t *shell = flux_plugin_get_shell (p);
    struct fence *fx;
    flux_plugin_arg_t *args;
    const char *name = NULL;

    if (!(fx = calloc (1, sizeof (*fx))))
        return NULL;
    fx->shell = shell;
    fx->it = it;
    fx->trace_flag = 1; // stuck on for now
    if (flux_shell_info_unpack (shell,
                                "{s:i s:i}",
                                "rank", &fx->shell_rank,
                                "size", &fx->shell_size) < 0)
        goto error;
    if (!(fx->taskmap = flux_shell_get_taskmap (shell)))
        goto error;
    snprintf (fx->key, sizeof (fx->key), "%d", fx->shell_rank);
    if (interthread_register (it, "fence_upcall", fence_shell_cb, fx) < 0)
        goto error;
    if (flux_plugin_add_handler (p,
                                 FENCE_EXIT_TOPIC,
                                 exchange_exit_cb,
                                 fx) < 0)
        goto error;
    /* flux_shell_plugstack_call() succeeds when no plugin handles the
     * topic.  The jobdict plugin sets name in the OUT args to show that
     * it handled jobdict.create.  If name is not set, the flux-shell has
     * no jobdict plugin.  flux-core 0.91.0 added it.
     */
    if (jobdict_call (fx,
                      "jobdict.create",
                      &args,
                      "{s:s s:i}",
                      "name", FENCE_JOBDICT,
                      "k", k) < 0)
        goto error;
    (void)flux_plugin_arg_unpack (args,
                                  FLUX_PLUGIN_ARG_OUT,
                                  "{s:s}",
                                  "name", &name);
    flux_plugin_arg_destroy (args);
    if (!name) {
        shell_log_error ("jobdict plugin not found (need flux-core >= 0.91.0)");
        errno = ENOSYS;
        goto error;
    }
    global_fence_ctx = fx;
    return fx;
error:
    fence_destroy (fx);
    return NULL;
}

// vi:ts=4 sw=4 expandtab
