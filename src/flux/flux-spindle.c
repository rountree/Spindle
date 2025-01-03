/*
 * Spindle job shell plugin for Flux.
 */
#include <blr_log.h>
#define _GNU_SOURCE
#include <sys/types.h>
#include <signal.h>
#include <stdio.h>
#include <jansson.h>
#include <strings.h>

#define FLUX_SHELL_PLUGIN_NAME "spindle"

#include <flux/core.h>
#include <flux/shell.h>
#include <flux/hostlist.h>

#include <spindle_launch.h>

struct spindle_ctx {
    spindle_args_t params;   /* Spindle parameters                        */
    int flags;               /* Spindle args initialzation flags          */
    pid_t backend_pid;       /* pid of spindle backend                    */
    int argc;                /* argc of args to prepend to job cmdline    */
    char **argv;             /* argv to prepend to job cmdline            */

    int shell_rank;          /* This shell rank                           */
    flux_jobid_t id;         /* jobid                                     */

    char **hosts;            /* Hostlist from R expanded to array         */
};

/* Free a malloc'd array of malloc'd char *
 */
static void free_argv (char **argv)
{
    blr_preamble();
    if (argv) {
        char **s;
        for (s = argv; *s != NULL; s++)
            free (*s);
        free (argv);
    }
    blr_postamble();
}

/* Convert the hostlist in an Rv1 object to an array of hostnames
 */
static char **R_to_hosts (json_t *R)
{
    blr_preamble();
    struct hostlist *hl = hostlist_create ();
    json_t *nodelist;
    size_t index;
    json_t *entry;
    const char *host;
    char **hosts = NULL;
    int i;

    if (json_unpack (R,
                     "{s:{s:o}}",
                     "execution",
                     "nodelist", &nodelist) < 0){
        goto error;
    }

    json_array_foreach (nodelist, index, entry) {
        const char *val = json_string_value (entry);
        if (!val || hostlist_append (hl, val) < 0){
            goto error;
        }
    }
    if (!(hosts = calloc (hostlist_count (hl) + 1, sizeof (char *)))){
        goto error;
    }
    host = hostlist_first (hl);
    i = 0;
    while (host) {
        if (!(hosts[i] = strdup (host)))
            goto error;
        host = hostlist_next (hl);
        i++;
    }
    hostlist_destroy (hl);
    blr_postamble();
    return hosts;
error:
    free_argv (hosts);
    hostlist_destroy (hl);
    blr_postamble();
    return NULL;
}


/*  Create a spindle plugin ctx from jobid 'id', shell rank 'rank',
 *   and an Rv1 json object.
 */
static struct spindle_ctx *spindle_ctx_create (flux_jobid_t id,
                                               int rank,
                                               json_t *R)
{
    blr_preamble();
    struct spindle_ctx *ctx = calloc (1, sizeof (*ctx));
    if (!ctx){
        blr_postamble();
        return NULL;
    }
    ctx->id = id;
    ctx->shell_rank = rank;

    if (!(ctx->hosts = R_to_hosts (R))) {
        free (ctx);
        blr_postamble();
        return NULL;
    }

    /*  This spindle_args_t number must be shared across all shell ranks
     *   as well as unique among any simultaneous spindle sessions. Therefore,
     *   derive from the jobid, which should be unique enough within a job.
     */
    ctx->params.number = (unsigned int) (id & 0xffffffff);

    /*  unique_id is 64 bits so we can use the jobid
     *  N.B. Hangs are seen if this isn't also set after the call to
     *   initialize args, see comment in sp_init().
     */
    ctx->params.unique_id = (unique_id_t) id;

    /*  This flag prevents spindle from regenerating the unique id and
     *   `number` in ctx->params.
     */
    ctx->flags = SPINDLE_FILLARGS_NONUMBER | SPINDLE_FILLARGS_NOUNIQUEID;

    blr_postamble();
    return ctx;
}

static void spindle_ctx_destroy (struct spindle_ctx *ctx)
{
    blr_preamble();
    if (ctx) {
        int saved_errno = errno;
        free_argv (ctx->argv);
        free_argv (ctx->hosts);
        free (ctx);
        errno = saved_errno;
    }
    blr_postamble();
}

/*  Run spindle backend as a child of the shell
 */
static int run_spindle_backend (struct spindle_ctx *ctx)
{
    blr_preamble();
    ctx->backend_pid = fork ();
    if (ctx->backend_pid == 0) {
        /* N.B.: spindleRunBE() blocks, which is why we run it in a child
         */
        if (spindleRunBE (ctx->params.port,
                          ctx->params.num_ports,
                          ctx->id,
                          OPT_SEC_MUNGE,
                          NULL) < 0) {
            fprintf (stderr, "spindleRunBE failed!\n");
            blr_postamble();
            exit (1);
        }
        blr_postamble();
        exit (0);
    }
    shell_debug ("started spindle backend pid = %u", ctx->backend_pid);
    blr_postamble();
    return 0;
}

/*  Run spindle frontend. Only in shell rank 0.
 */
static void run_spindle_frontend (struct spindle_ctx *ctx)
{
    blr_preamble();
        /* Blocks untile backends connect */
        if (spindleInitFE ((const char **) ctx->hosts, &ctx->params) < 0){
            blr_postamble();
            shell_die (1, "spindleInitFE");
        }
        shell_debug ("started spindle frontend");
    blr_postamble();
}

/*  Callback for watching the exec eventlog
 *  Upon seeing the shell.init event, parse the spindle port and num_ports,
 *   then start backend and frontend on rank 0.
 */
static void wait_for_shell_init (flux_future_t *f, void *arg)
{
    blr_preamble();
    struct spindle_ctx *ctx = arg;
    json_t *o;
    const char *event;
    const char *name;
    int rc = -1;

    if (ctx->params.opts & OPT_OFF) {
       blr_postamble();
       return;
    }
    
    if (flux_job_event_watch_get (f, &event) < 0)
        shell_die_errno (1, "spindle failed waiting for shell.init event");
    if (!(o = json_loads (event, 0, NULL))
            || json_unpack (o, "{s:s}", "name", &name) < 0)
        shell_die_errno (1, "failed to get event name");
    if (strcmp (name, "shell.init") == 0) {
        rc = json_unpack (o,
                "{s:{s:i s:i}}",
                "context",
                "spindle_port", &ctx->params.port,
                "spindle_num_ports", &ctx->params.num_ports);
    }
    json_decref (o);
    if (rc != 0) {
        flux_future_reset (f);
        blr_postamble();
        return;
    }
    flux_future_destroy (f);

    /*  Now that port and num_ports are obtained from rank 0, start
     *   the backends and frontend on rank 0
     */
    run_spindle_backend (ctx);

    if (ctx->shell_rank == 0){
        run_spindle_frontend (ctx);
    }
    blr_postamble();
}

static int parse_yesno(opt_t *opt, opt_t flag, const char *yesno)
{
    blr_preamble();
    if (strcasecmp(yesno, "no") == 0 || strcasecmp(yesno, "false") == 0 || strcasecmp(yesno, "0") == 0){
        *opt &= ~flag;
    }else if (strcasecmp(yesno, "yes") == 0 || strcasecmp(yesno, "true") == 0 || strcasecmp(yesno, "1") == 0){
        *opt |= 1;
    }else{
        int rc = shell_log_errno ("Error in spindle option: Expected 'yes' or 'no', got %s", yesno);
        blr_postamble();
        return rc;
    }
    blr_postamble();
    return 0;
}

static int sp_getopts (flux_shell_t *shell, struct spindle_ctx *ctx)
{
    blr_preamble();
    json_error_t error;
    json_t *opts;
    int noclean = 0;
    int nostrip = 0;
    int follow_fork = 0;
    int push = 0;
    int pull = 0;
    int had_error = 0;
    int numa = 0;
    const char *relocaout = NULL, *reloclibs = NULL, *relocexec = NULL, *relocpython = NULL;
    const char *followfork = NULL, *preload = NULL, *level = NULL;
    const char *pyprefix = NULL;
    char *numafiles = NULL;

    if (flux_shell_getopt_unpack (shell, "spindle", "o", &opts) < 0){
        blr_postamble();
        return -1;
    }

    /*
     * Options we need to be always on
     */
    ctx->params.opts |= OPT_PERSIST;

    /*  attributes.system.shell.options.spindle=1 is valid if no other
     *  spindle options are set. Return early if this is the case.
     */
    if (json_is_integer (opts) && json_integer_value (opts) > 0){
        blr_postamble();
        return 0;
    }

    /*  O/w, unpack extra spindle options from the options.spindle JSON
     *  object. To support more options, add them to the unpack below:
     *  Note that it is an error if extra options not handled here are
     *  supplied by the user, but not unpacked (This handles typos, etc).
     */
    if (json_unpack_ex (opts, &error, JSON_STRICT,
                        "{s?i s?i s?i s?i s?s s?s s?s s?s s?s s?s s?i s?s s?s s?s}",
                        "noclean", &noclean,
                        "nostrip", &nostrip,
                        "push", &push,
                        "pull", &pull,
                        "reloc-aout", &relocaout,
                        "follow-fork", &followfork,
                        "reloc-libs", &reloclibs,
                        "reloc-exec", &relocexec,
                        "reloc-python", &relocpython,
                        "python-prefix", &pyprefix,
                        "numa", &numa,
                        "numa-files", &numafiles,
                        "preload", &preload,
                        "level", &level) < 0){
        blr_postamble();
        return shell_log_errno ("Error in spindle option: %s", error.text);
    }

    if (noclean){
        ctx->params.opts |= OPT_NOCLEAN;
    }
    if (nostrip){
        ctx->params.opts &= ~OPT_STRIP;
    }
    if (follow_fork){
        ctx->params.opts |= OPT_FOLLOWFORK;
    }
    if (push) {
       ctx->params.opts |= OPT_PUSH;
       ctx->params.opts &= ~OPT_PULL;
    }
    if (pull) {
       ctx->params.opts &= ~OPT_PUSH;
       ctx->params.opts |= OPT_PULL;
    }
    if (relocaout){
       had_error |= parse_yesno(&ctx->params.opts, OPT_RELOCAOUT, relocaout);
    }
    if (followfork){
       had_error |= parse_yesno(&ctx->params.opts, OPT_FOLLOWFORK, followfork);
    }
    if (reloclibs){
       had_error |= parse_yesno(&ctx->params.opts, OPT_RELOCSO, reloclibs);
    }
    if (relocexec){
       had_error |= parse_yesno(&ctx->params.opts, OPT_RELOCEXEC, relocexec);
    }
    if (relocpython){
       had_error |= parse_yesno(&ctx->params.opts, OPT_RELOCPY, relocpython);
    }
    if (preload){
       ctx->params.preloadfile = (char *) preload;
    }
    if (numa) {
       ctx->params.opts |= OPT_NUMA;
    }
    if (numafiles) {
       ctx->params.opts |= OPT_NUMA;
       ctx->params.numa_files = numafiles;
    }
    if (pyprefix) {
        char *tmp;
        if (asprintf (&tmp, "%s:%s", ctx->params.pythonprefix, pyprefix) < 0){
            int rc = shell_log_errno ("unable to append to pythonprefix");
            blr_postamble();
            return rc;
        }
        free (ctx->params.pythonprefix);
        ctx->params.pythonprefix = tmp;
    }
    if (level) {
       if (strcmp(level, "high") == 0) {
          ctx->params.opts |= OPT_RELOCAOUT;
          ctx->params.opts |= OPT_RELOCSO;
          ctx->params.opts |= OPT_RELOCPY;
          ctx->params.opts |= OPT_RELOCEXEC;
          ctx->params.opts |= OPT_FOLLOWFORK;
          ctx->params.opts &= ~((opt_t) OPT_STOPRELOC);
          ctx->params.opts &= ~((opt_t) OPT_OFF);
       }
       if (strcmp(level, "medium") == 0) {
          ctx->params.opts &= ~((opt_t) OPT_RELOCAOUT);
          ctx->params.opts |= OPT_RELOCSO;
          ctx->params.opts |= OPT_RELOCPY;
          ctx->params.opts &= ~((opt_t) OPT_RELOCEXEC);
          ctx->params.opts |= OPT_FOLLOWFORK;
          ctx->params.opts &= ~((opt_t) OPT_STOPRELOC);
          ctx->params.opts &= ~((opt_t) OPT_OFF);
       }
       if (strcmp(level, "low") == 0) {
          ctx->params.opts &= ~((opt_t) OPT_RELOCAOUT);
          ctx->params.opts &= ~((opt_t) OPT_RELOCSO);
          ctx->params.opts &= ~((opt_t) OPT_RELOCPY);
          ctx->params.opts &= ~((opt_t) OPT_RELOCEXEC);
          ctx->params.opts |= OPT_FOLLOWFORK;
          ctx->params.opts |= OPT_STOPRELOC;
          ctx->params.opts &= ~((opt_t) OPT_OFF);
       }
       if (strcmp(level, "off") == 0) {
          ctx->params.opts &= ~((opt_t) OPT_RELOCAOUT);
          ctx->params.opts &= ~((opt_t) OPT_RELOCSO);
          ctx->params.opts &= ~((opt_t) OPT_RELOCPY);
          ctx->params.opts &= ~((opt_t) OPT_RELOCEXEC);
          ctx->params.opts |= OPT_FOLLOWFORK;
          ctx->params.opts &= ~((opt_t) OPT_STOPRELOC);
          ctx->params.opts |= OPT_OFF;
       }
    }
    if (had_error){
        blr_postamble();
        return had_error;
    }
    blr_postamble();
    return 0;
}

/*  Spindle plugin shell.init callback
 *  Initialize spindle params and other context. On rank 0, add the
 *   port and num_ports to the shell.init event.
 */
static int sp_init (flux_plugin_t *p,
                    const char *topic,
                    flux_plugin_arg_t *arg,
                    void *data)
{
    blr_preamble();
    struct spindle_ctx *ctx;
    flux_shell_t *shell = flux_plugin_get_shell (p);
    flux_t *h = flux_shell_get_flux (shell);
    flux_jobid_t id;
    int shell_rank;
    flux_future_t *f;
    json_t *R;
    const char *debug;
    const char *tmpdir;
    const char *test;

    if (!(shell = flux_plugin_get_shell (p))
        || !(h = flux_shell_get_flux (shell))){
        blr_postamble();
        return shell_log_errno ("failed to get shell or flux handle");
    }

    if (flux_shell_getopt (shell, "spindle", NULL) != 1){
        blr_postamble();
        return 0;
    }

    shell_debug ("initializing spindle for use with flux");

    /*  If SPINDLE_DEBUG is set in the environment of the job, propagate
     *  it into the shell so we get spindle debugging for this session.
     */
    if ((debug = flux_shell_getenv (shell, "SPINDLE_DEBUG"))){
        setenv ("SPINDLE_DEBUG", debug, 1);
    }

    /*  The spindle testsuite requires SPINDLE_TEST
     */
    if ((test = flux_shell_getenv (shell, "SPINDLE_TEST"))){
       setenv ("SPINDLE_TEST", test, 1);
    }

    /*  Spindle requires that TMPDIR is set. Propagate TMPDIR from job
     *  environment, or use /tmp if TMPDIR not set.
     */
    tmpdir = flux_shell_getenv (shell, "TMPDIR");
    if (!tmpdir){
        tmpdir = "/tmp";
    }
    setenv ("TMPDIR", tmpdir, 1);

    /*  Get the jobid, R, and shell rank
     */
    if (flux_shell_info_unpack (shell,
                                "{s:I s:o s:i}",
                                "jobid", &id,
                                "R", &R,
                                "rank", &shell_rank) < 0){
        int rc = shell_log_errno ("Failed to unpack shell info");
        blr_postamble();
        return rc;
    }
    blr_log( "shell_rank = %i\n", shell_rank );
    /*  Create an object for spindle related context.
     *
     *  Set this object in the plugin context for later fetching as
     *   well as auto-destruction on plugin unload.
     */
    if (!(ctx = spindle_ctx_create (id, shell_rank, R))
        || flux_plugin_aux_set (p,
                                "spindle",
                                ctx,
                                (flux_free_f) spindle_ctx_destroy) < 0) {
        spindle_ctx_destroy (ctx);
        int rc = shell_log_errno ("failed to create spindle ctx");
        blr_postamble();
        return rc;
    }

    /*  Fill in the spindle_args_t with defaults from Spindle.
     *  We use fillInSpindleArgsCmdlineFE() here so that spindle does
     *   not overwrite our already-initialized `number`, which must be
     *   shared across the session.
     */
    if (fillInSpindleArgsCmdlineFE (&ctx->params,
                                    ctx->flags,
                                    0,
                                    NULL,
                                    NULL) < 0){
        int rc = shell_log_errno ("fillInSpindleArgsCmdlineFE failed");
        blr_postamble();
        return rc;
    }


    /*  Read other spindle options from spindle option in jobspec:
     */
    if (sp_getopts (shell, ctx) < 0){
        blr_postamble();
        return -1;
    }
    if (ctx->params.opts & OPT_OFF) {
        blr_postamble();
       return 0;
    }

    /*  N.B. Override unique_id with id again to be sure it wasn't changed
     *  (Occaisionally see hangs if this is not done)
     */
    ctx->params.unique_id = id;

    /*  Get args to prepend to job cmdline
     */
    if (getApplicationArgsFE(&ctx->params, &ctx->argc, &ctx->argv) < 0){
        blr_postamble();
        shell_die (1, "getApplicationArgsFE");
    }

    if (shell_rank == 0) {
        /*  Rank 0: add spindle port and num_ports to the shell.init
         *   exec eventlog event. All other shell's will wait for this
         *   event and initialize their port/num_ports from these values.
         */
        flux_shell_add_event_context (shell, "shell.init", 0,
                                      "{s:i s:i}",
                                      "spindle_port",
                                      ctx->params.port,
                                      "spindle_num_ports",
                                      ctx->params.num_ports);
    }

    /*  All ranks, watch guest.exec.eventlog for the shell.init event in
     *   order to distribute port and num_ports. This is unnecessary on
     *   rank 0, but code is simpler if we treat all ranks the same.
     */
    if (!(f = flux_job_event_watch (h, id, "guest.exec.eventlog", 0))
        || flux_future_then (f, 300., wait_for_shell_init, ctx) < 0){
        blr_postamble();
        shell_die (1, "flux_job_event_watch");
    }

    /*  Return control to job shell */
    blr_postamble();
    return 0;
}

/*  task.init plugin callback.
 *
 *  This callback will be called before the shell executes the job tasks.
 *  Modify the task commandline with spindle argv if necessary.
 */
static int sp_task (flux_plugin_t *p,
                    const char *topic,
                    flux_plugin_arg_t *arg,
                    void *data)
{
    blr_preamble();
    struct spindle_ctx *ctx = flux_plugin_aux_get (p, "spindle");
    if (ctx && ctx->argc > 0 && !(ctx->params.opts & OPT_OFF)) {
        int i;
        flux_shell_t *shell = flux_plugin_get_shell (p);
        flux_shell_task_t *task = flux_shell_current_task (shell);
        flux_cmd_t *cmd = flux_shell_task_cmd (task);

        /* Prepend spindle_argv to task cmd */
        for (i = ctx->argc - 1; i >= 0; i--)
            flux_cmd_argv_insert (cmd, 0, ctx->argv[i]);

        char *s = flux_cmd_stringify (cmd);
        shell_trace ("running %s", s);
        free (s);
    }
    blr_postamble();
    return 0;
}

/*  Shell exit handler.
 *  Close the frontend on rank 0. All ranks terminate the backend.
 */
static int sp_exit (flux_plugin_t *p,
                    const char *topic,
                    flux_plugin_arg_t *arg,
                    void *data)
{
    blr_preamble();
    struct spindle_ctx *ctx = flux_plugin_aux_get (p, "spindle");
    if (ctx->params.opts & OPT_OFF){
        blr_postamble();
       return 0;
    }
    if (ctx && ctx->shell_rank == 0){
        blr_postamble();
        spindleCloseFE (&ctx->params);
    }
    blr_postamble();
    return 0;
}

int flux_plugin_init (flux_plugin_t *p)
{
    blr_preamble();
    if (flux_plugin_set_name (p, "spindle") < 0
        || flux_plugin_add_handler (p, "shell.init", sp_init, NULL) < 0
        || flux_plugin_add_handler (p, "task.init",  sp_task, NULL) < 0
        || flux_plugin_add_handler (p, "shell.exit", sp_exit, NULL) < 0){
        blr_postamble();
        return -1;
    }
    blr_postamble();
    return 0;
}


