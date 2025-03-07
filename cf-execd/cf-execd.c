/*
  Copyright 2023 Northern.tech AS

  This file is part of CFEngine 3 - written and maintained by Northern.tech AS.

  This program is free software; you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by the
  Free Software Foundation; version 3.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA

  To the extent this program is licensed as part of the Enterprise
  versions of CFEngine, the applicable Commercial Open Source License
  (COSL) may apply to this file if you as a licensee so wish it. See
  included file COSL.txt.
*/

#include <cf-execd.h>

#ifndef __MINGW32__
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <files_lib.h>
#include <cf-execd-runagent.h>
#include <files_names.h>        /* ChopLastNode */

#ifndef AF_LOCAL
#define AF_LOCAL AF_UNIX
#endif

#endif

#include <cf-execd-runner.h>
#include <item_lib.h>
#include <known_dirs.h>
#include <man.h>
#include <ornaments.h>
#include <exec_tools.h>
#include <signals.h>
#include <processes_select.h>
#include <bootstrap.h>
#include <policy_server.h>
#include <sysinfo.h>
#include <timeout.h>
#include <time_classes.h>
#include <loading.h>
#include <printsize.h>
#include <cleanup.h>
#include <repair.h>
#include <dbm_api.h>            /* CheckDBRepairFlagFile() */
#include <string_lib.h>
#include <acl_tools.h>          /* AllowAccessForUsers() */

#include <cf-windows-functions.h>

#define CF_EXEC_IFELAPSED 0
#define CF_EXEC_EXPIREAFTER 1

#define CF_EXECD_RUNAGENT_SOCKET_NAME "runagent.socket"

/* The listen() queue doesn't need to be long, new connections are accepted
 * quickly and handed over to forked child processes so a pile up means some
 * serious problem and it's better to just throw such connections away. */
#define CF_EXECD_RUNAGENT_SOCKET_LISTEN_QUEUE 5

static bool PERFORM_DB_CHECK = false;
static int NO_FORK = false; /* GLOBAL_A */
static int ONCE = false; /* GLOBAL_A */
static int WINSERVICE = true; /* GLOBAL_A */

static char *RUNAGENT_SOCKET_DIR = NULL;

static pthread_attr_t threads_attrs; /* GLOBAL_T, initialized by pthread_attr_init */

/*******************************************************************/

static GenericAgentConfig *CheckOpts(int argc, char **argv);

void ThisAgentInit(void);
static bool ScheduleRun(EvalContext *ctx, Policy **policy, GenericAgentConfig *config,
                        ExecdConfig **execd_config, ExecConfig **exec_config);
#ifndef __MINGW32__
static pid_t LocalExecInFork(const ExecConfig *config);
static void Apoptosis(void);
static inline bool GetRunagentSocketInfo(struct sockaddr_un *sock_info);
static inline bool SetRunagentSocketACLs(char *sock_path, StringSet *allow_users);
#else
static bool LocalExecInThread(const ExecConfig *config);
#endif


/*******************************************************************/
/* Command line options                                            */
/*******************************************************************/

static const char *const CF_EXECD_SHORT_DESCRIPTION =
    "scheduling daemon for cf-agent";

static const char *const CF_EXECD_MANPAGE_LONG_DESCRIPTION =
    "cf-execd is the scheduling daemon for cf-agent. It runs cf-agent locally according to a schedule specified in "
    "policy code (executor control body). After a cf-agent run is completed, cf-execd gathers output from cf-agent, "
    "and may be configured to email the output to a specified address. It may also be configured to splay (randomize) the "
    "execution schedule to prevent synchronized cf-agent runs across a network. "
    "Note: this daemon reloads it's config when the SIGHUP signal is received.";

static const struct option OPTIONS[] =
{
    {"help", no_argument, 0, 'h'},
    {"debug", no_argument, 0, 'd'},
    {"verbose", no_argument, 0, 'v'},
    {"dry-run", no_argument, 0, 'n'},
    {"version", no_argument, 0, 'V'},
    {"file", required_argument, 0, 'f'},
    {"define", required_argument, 0, 'D'},
    {"negate", required_argument, 0, 'N'},
    {"no-lock", no_argument, 0, 'K'},
    {"inform", no_argument, 0, 'I'},
    {"diagnostic", no_argument, 0, 'x'},
    {"log-level", required_argument, 0, 'g'},
    {"no-fork", no_argument, 0, 'F'},
    {"once", no_argument, 0, 'O'},
    {"no-winsrv", no_argument, 0, 'W'},
    {"ld-library-path", required_argument, 0, 'L'},
    {"color", optional_argument, 0, 'C'},
    {"timestamp", no_argument, 0, 'l'},
    /* Only long option for the rest */
    {"ignore-preferred-augments", no_argument, 0, 0},
    {"skip-db-check", optional_argument, 0, 0 },
    {"with-runagent-socket", required_argument, 0, 0},
    {NULL, 0, 0, '\0'}
};

static const char *const HINTS[] =
{
    "Print the help message",
    "Enable debugging output",
    "Output verbose information about the behaviour of cf-execd",
    "All talk and no action mode - make no changes, only inform of promises not kept",
    "Output the version of the software",
    "Specify an alternative input file than the default. This option is overridden by FILE if supplied as argument.",
    "Define a list of comma separated classes to be defined at the start of execution",
    "Define a list of comma separated classes to be undefined at the start of execution",
    "Ignore locking constraints during execution (ifelapsed/expireafter) if \"too soon\" to run",
    "Print basic information about changes made to the system, i.e. promises repaired",
    "Activate internal diagnostics (developers only)",
    "Specify how detailed logs should be. Possible values: 'error', 'warning', 'notice', 'info', 'verbose', 'debug'",
    "Run as a foreground processes (do not fork)",
    "Run once and then exit (implies no-fork)",
    "Do not run as a service on windows - use this when running from a command shell (CFEngine Nova only)",
    "Set the internal value of LD_LIBRARY_PATH for child processes",
    "Enable colorized output. Possible values: 'always', 'auto', 'never'. If option is used, the default value is 'auto'",
    "Log timestamps on each line of log output",
    "Ignore def_preferred.json file in favor of def.json",
    "Do not run database integrity checks and repairs at startup",
    "Specify the directory for the socket for runagent requests or 'no' to disable the socket",
    NULL
};

/*****************************************************************************/

int main(int argc, char *argv[])
{
    GenericAgentConfig *config = CheckOpts(argc, argv);
    bool force_repair = CheckDBRepairFlagFile();
    if (force_repair || PERFORM_DB_CHECK)
    {
        repair_lmdb_default(force_repair);
    }

    EvalContext *ctx = EvalContextNew();
    GenericAgentConfigApply(ctx, config);

    const char *program_invocation_name = argv[0];
    const char *last_dir_sep = strrchr(program_invocation_name, FILE_SEPARATOR);
    const char *program_name = (last_dir_sep != NULL ? last_dir_sep + 1 : program_invocation_name);
    GenericAgentDiscoverContext(ctx, config, program_name);

    Policy *policy = SelectAndLoadPolicy(config, ctx, false, false);

    if (!policy)
    {
        Log(LOG_LEVEL_ERR, "Error reading CFEngine policy. Exiting...");
        DoCleanupAndExit(EXIT_FAILURE);
    }

    GenericAgentPostLoadInit(ctx);
    ThisAgentInit();

    ExecConfig *exec_config = ExecConfigNew(!ONCE, ctx, policy);
    ExecdConfig *execd_config = ExecdConfigNew(ctx, policy);
    SetFacility(execd_config->log_facility);

#ifdef __MINGW32__
    if (WINSERVICE)
    {
        NovaWin_StartExecService();
    }
    else
#endif /* __MINGW32__ */
    {
        StartServer(ctx, policy, config, &execd_config, &exec_config);
    }

    GenericAgentFinalize(ctx, config);
    ExecConfigDestroy(exec_config);
    ExecdConfigDestroy(execd_config);
    free(RUNAGENT_SOCKET_DIR);
    CallCleanupFunctions();
    return 0;
}

/*****************************************************************************/
/* Level 1                                                                   */
/*****************************************************************************/

static GenericAgentConfig *CheckOpts(int argc, char **argv)
{
    extern char *optarg;
    int c;

    GenericAgentConfig *config = GenericAgentConfigNewDefault(AGENT_TYPE_EXECUTOR, GetTTYInteractive());


    int longopt_idx;
    while ((c = getopt_long(argc, argv, "dvnKIf:g:D:N:VxL:hFOV1gMWC::l",
                            OPTIONS, &longopt_idx))
           != -1)
    {
        switch (c)
        {
        case 'f':
            GenericAgentConfigSetInputFile(config, GetInputDir(), optarg);
            MINUSF = true;
            break;

        case 'd':
            LogSetGlobalLevel(LOG_LEVEL_DEBUG);
            break;

        case 'K':
            config->ignore_locks = true;
            break;

        case 'D':
            {
                StringSet *defined_classes = StringSetFromString(optarg, ',');
                if (! config->heap_soft)
                {
                    config->heap_soft = defined_classes;
                }
                else
                {
                    StringSetJoin(config->heap_soft, defined_classes, xstrdup);
                    StringSetDestroy(defined_classes);
                }
            }
            break;

        case 'N':
            {
                StringSet *negated_classes = StringSetFromString(optarg, ',');
                if (! config->heap_negated)
                {
                    config->heap_negated = negated_classes;
                }
                else
                {
                    StringSetJoin(config->heap_negated, negated_classes, xstrdup);
                    StringSetDestroy(negated_classes);
                }
            }
            break;

        case 'I':
            LogSetGlobalLevel(LOG_LEVEL_INFO);
            break;

        case 'v':
            LogSetGlobalLevel(LOG_LEVEL_VERBOSE);
            NO_FORK = true; // TODO: really?
            break;

        case 'g':
            LogSetGlobalLevelArgOrExit(optarg);
            break;

        case 'n':
            EVAL_MODE = EVAL_MODE_DRY_RUN;
            config->ignore_locks = true;
            break;

        case 'L':
            {
                Log(LOG_LEVEL_VERBOSE, "Setting 'LD_LIBRARY_PATH=%s'", optarg);
                setenv_wrapper("LD_LIBRARY_PATH", optarg, 1);
                break;
            }
        case 'W':
            WINSERVICE = false;
            break;

        case 'F':
            NO_FORK = true;
            break;

        case 'O':
            ONCE = true;
            NO_FORK = true;
            break;

        case 'V':
        {
            Writer *w = FileWriter(stdout);
            GenericAgentWriteVersion(w);
            FileWriterDetach(w);
        }
        DoCleanupAndExit(EXIT_SUCCESS);

        case 'h':
        {
            Writer *w = FileWriter(stdout);
            WriterWriteHelp(w, "cf-execd", OPTIONS, HINTS, NULL, false, true);
            FileWriterDetach(w);
        }
        DoCleanupAndExit(EXIT_SUCCESS);

        case 'M':
        {
            Writer *out = FileWriter(stdout);
            ManPageWrite(out, "cf-execd", time(NULL),
                         CF_EXECD_SHORT_DESCRIPTION,
                         CF_EXECD_MANPAGE_LONG_DESCRIPTION,
                         OPTIONS, HINTS,
                         NULL, false,
                         true);
            FileWriterDetach(out);
            DoCleanupAndExit(EXIT_SUCCESS);
        }

        case 'x':
            Log(LOG_LEVEL_ERR, "Self-diagnostic functionality is retired.");
            DoCleanupAndExit(EXIT_SUCCESS);

        case 'C':
            if (!GenericAgentConfigParseColor(config, optarg))
            {
                DoCleanupAndExit(EXIT_FAILURE);
            }
            break;

        case 'l':
            LoggingEnableTimestamps(true);
            break;

        case 0:
        {
            const char *const option_name = OPTIONS[longopt_idx].name;
            if (StringEqual(option_name, "ignore-preferred-augments"))
            {
                config->ignore_preferred_augments = true;
            }
            else if (StringEqual(option_name, "skip-db-check"))
            {
                if (optarg == NULL)
                {
                    PERFORM_DB_CHECK = false; // Skip (no arg), check = false
                }
                else if (StringEqual_IgnoreCase(optarg, "yes"))
                {
                    PERFORM_DB_CHECK = false; // Skip = yes, check = false
                }
                else if (StringEqual_IgnoreCase(optarg, "no"))
                {
                    PERFORM_DB_CHECK = true; // Skip = no, check = true
                }
                else
                {
                    Log(LOG_LEVEL_ERR,
                        "Invalid argument for --skip-db-check(yes/no): '%s'",
                        optarg);
                    DoCleanupAndExit(EXIT_FAILURE);
                }
            }
            else if (StringEqual(option_name, "with-runagent-socket"))
            {
                assert(optarg != NULL); /* required_argument */
                RUNAGENT_SOCKET_DIR = xstrdup(optarg);
            }

            break;
        }
        default:
        {
            Writer *w = FileWriter(stdout);
            WriterWriteHelp(w, "cf-execd", OPTIONS, HINTS, NULL, false, true);
            FileWriterDetach(w);
        }
        DoCleanupAndExit(EXIT_FAILURE);

        }
    }

    if (!GenericAgentConfigParseArguments(config, argc - optind, argv + optind))
    {
        Log(LOG_LEVEL_ERR, "Too many arguments");
        DoCleanupAndExit(EXIT_FAILURE);
    }

    return config;
}

/*****************************************************************************/

void ThisAgentInit(void)
{
    umask(077);
}

/*****************************************************************************/


#ifndef __MINGW32__

static inline bool UsingRunagentSocket()
{
    /* No runagent socket dir specified (use the default) or a directory
     * specified ("no" disables the functionality). */
    return ((RUNAGENT_SOCKET_DIR == NULL) || (!StringEqual_IgnoreCase(RUNAGENT_SOCKET_DIR, "no")));
}

/**
 * Sleep for the given number of seconds while handling requests from sockets.
 *
 * @return Whether to terminate (skip any further actions) or not.
 */
static bool HandleRequestsOrSleep(time_t seconds, const char *reason,
                                  int runagent_socket, const char *local_run_command)
{
    if (IsPendingTermination())
    {
        return true;
    }

    Log(LOG_LEVEL_VERBOSE, "Sleeping for %s %ju seconds", reason, (intmax_t) seconds);

    if (runagent_socket >= 0)
    {
        time_t sleep_started = time(NULL);
        struct timeval remaining = {seconds, 0};
        while (remaining.tv_sec != 0)
        {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(runagent_socket, &rfds);

            int ret = select(runagent_socket + 1, &rfds, NULL, NULL, &remaining);
            if ((ret == -1) && (errno != EINTR))
            {
                /* unexpected error */
                Log(LOG_LEVEL_ERR, "Failed to sleep for %s using select(): %s",
                    reason, GetErrorStr());
            }
            else if (ret == 0)
            {
                /* timeout -- slept for the specified time */
                remaining.tv_sec = 0;
            }
            else
            {
                /* runagent_socket ready or signal received (EINTR) */

                // We are sleeping above, so make sure a terminating signal did not
                // arrive during that time.
                if (IsPendingTermination())
                {
                    return true;
                }

                if (ret > 0)
                {
                    assert(FD_ISSET(runagent_socket, &rfds));
                    int data_socket = accept(runagent_socket, NULL, NULL);
                    pid_t pid = fork();
                    if (pid == 0)
                    {
                        /* child */
                        signal(SIGPIPE, SIG_DFL);
                        HandleRunagentRequest(data_socket, local_run_command);
                        _exit(EXIT_SUCCESS);
                    }
                    else if (pid == -1)
                    {
                        /* error */
                        Log(LOG_LEVEL_ERR, "Failed to fork runagent request handler: %s",
                            GetErrorStr());
                    }
                    /* parent: nothing more to do, go back to sleep */
                }

                remaining.tv_sec = MAX(0, seconds - (time(NULL) - sleep_started));
            }
        }
    }
    else
    {
        sleep(seconds);
    }

    // We are sleeping above, so make sure a terminating signal did not
    // arrive during that time.
    if (IsPendingTermination())
    {
        return true;
    }

    return false;
}

static void CFExecdMainLoop(EvalContext *ctx, Policy **policy, GenericAgentConfig *config,
                            ExecdConfig **execd_config, ExecConfig **exec_config,
                            int runagent_socket)
{
    bool terminate = false;
    while (!IsPendingTermination())
    {
        /* reap child processes (if any) */
        while (waitpid(-1, NULL, WNOHANG) > 0)
        {
            Log(LOG_LEVEL_DEBUG, "Reaped child process");
        }

        if (ScheduleRun(ctx, policy, config, execd_config, exec_config))
        {
            terminate = HandleRequestsOrSleep((*execd_config)->splay_time, "splay time",
                                              runagent_socket, (*execd_config)->local_run_command);
            if (terminate)
            {
                break;
            }
            pid_t child_pid = LocalExecInFork(*exec_config);
            if (child_pid < 0)
            {
                Log(LOG_LEVEL_INFO,
                    "Unable to run agent in a fork, falling back to blocking execution");
                LocalExec(*exec_config);
            }
        }
        /* 1 Minute resolution is enough */
        terminate = HandleRequestsOrSleep(CFPULSETIME, "pulse time", runagent_socket,
                                          (*execd_config)->local_run_command);
        if (terminate)
        {
            break;
        }
    }

    /* Remove the runagent socket (if any). */
    if (UsingRunagentSocket())
    {
        struct sockaddr_un sock_info;
        if (GetRunagentSocketInfo(&sock_info))
        {
            unlink(sock_info.sun_path);
        }
    }
}

static inline bool GetRunagentSocketInfo(struct sockaddr_un *sock_info)
{
    assert(sock_info != NULL);

    memset(sock_info, 0, sizeof(*sock_info));

    /* This can easily fail if GetStateDir() returns some long path,
     * 'sock_info.sun_path' is limited to 140 characters or even
     * fewer. "/var/cfengine/state" is fine, crazy long temporary state
     * directories used in the tests are too long. */
    int ret;
    if (RUNAGENT_SOCKET_DIR == NULL)
    {
        ret = snprintf(sock_info->sun_path, sizeof(sock_info->sun_path) - 1,
                       "%s/cf-execd.sockets/"CF_EXECD_RUNAGENT_SOCKET_NAME, GetStateDir());
    }
    else
    {
        ret = snprintf(sock_info->sun_path, sizeof(sock_info->sun_path) - 1,
                       "%s/"CF_EXECD_RUNAGENT_SOCKET_NAME, RUNAGENT_SOCKET_DIR);
    }
    return ((ret > 0) && ((size_t) ret <= (sizeof(sock_info->sun_path) - 1)));
}

static inline bool SetRunagentSocketACLs(char *sock_path, StringSet *allow_users)
{
    /* Allow access to the socket (rw) */
    bool success = AllowAccessForUsers(sock_path, allow_users, true, false);

    /* Need to ensure access to the parent folder too (rx) */
    if (success)
    {
        ChopLastNode(sock_path);
        success = AllowAccessForUsers(sock_path, allow_users, false, true);
    }
    return success;
}

static int SetupRunagentSocket(const ExecdConfig *execd_config)
{
    assert(execd_config != NULL);

    int runagent_socket = -1;

    struct sockaddr_un sock_info;
    if (GetRunagentSocketInfo(&sock_info))
    {
        sock_info.sun_family = AF_LOCAL;

        bool created;
        MakeParentDirectory(sock_info.sun_path, true, &created);

        /* Make sure the permissions are correct if the directory was created
         * (note: this code doesn't run on Windows). */
        if (created)
        {
            char *last_slash = strrchr(sock_info.sun_path, '/');
            *last_slash = '\0';
            chmod(sock_info.sun_path, (mode_t) 0750);
            *last_slash = '/';
        }

        /* Remove potential left-overs from old processes. */
        unlink(sock_info.sun_path);

        runagent_socket = socket(AF_LOCAL, SOCK_STREAM, 0);
        assert(runagent_socket >= 0);
    }
    if (runagent_socket < 0)
    {
        Log(LOG_LEVEL_ERR, "Failed to create socket for runagent requests");
    }
    else
    {
        int ret = bind(runagent_socket, (const struct sockaddr *) &sock_info, sizeof(sock_info));
        assert(ret == 0);
        if (ret == -1)
        {
            Log(LOG_LEVEL_ERR, "Failed to bind the runagent socket: %s", GetErrorStr());
            close(runagent_socket);
            runagent_socket = -1;
        }
        else
        {
            ret = listen(runagent_socket, CF_EXECD_RUNAGENT_SOCKET_LISTEN_QUEUE);
            assert(ret == 0);
            if (ret == -1)
            {
                Log(LOG_LEVEL_ERR, "Failed to listen on runagent socket: %s", GetErrorStr());
                close(runagent_socket);
                runagent_socket = -1;
            }
        }
        if (StringSetSize(execd_config->runagent_allow_users) > 0)
        {
            bool success = SetRunagentSocketACLs(sock_info.sun_path,
                                                 execd_config->runagent_allow_users);
            if (!success)
            {
                Log(LOG_LEVEL_ERR,
                    "Failed to allow runagent_socket_allow_users users access the runagent socket");
                /* keep going anyway */
            }
        }
    }
    return runagent_socket;
}

#else  /* ! __MINGW32__ */

/**
 * Sleep if not pending termination and log a message.
 *
 * @note: #msg_format should include exactly one "%u".
 */
static inline unsigned int MaybeSleepLog(LogLevel level, const char *msg_format, unsigned int seconds)
{
    if (IsPendingTermination())
    {
        return seconds;
    }

    Log(level, msg_format, seconds);

    return sleep(seconds);
}

static void CFExecdMainLoop(EvalContext *ctx, Policy **policy, GenericAgentConfig *config,
                            ExecdConfig **execd_config, ExecConfig **exec_config,
                            ARG_UNUSED int runagent_socket)
{
    while (!IsPendingTermination())
    {
        if (ScheduleRun(ctx, policy, config, execd_config, exec_config))
        {
            MaybeSleepLog(LOG_LEVEL_VERBOSE,
                          "Sleeping for splaytime %u seconds",
                          (*execd_config)->splay_time);

            // We are sleeping above, so make sure a terminating signal did not
            // arrive during that time.
            if (IsPendingTermination())
            {
                break;
            }
            if (!LocalExecInThread(*exec_config))
            {
                Log(LOG_LEVEL_INFO,
                    "Unable to run agent in thread, falling back to blocking execution");
                LocalExec(*exec_config);
            }
        }
        /* 1 Minute resolution is enough */
        MaybeSleepLog(LOG_LEVEL_VERBOSE, "Sleeping for pulse time %u seconds...", CFPULSETIME);
    }
}
#endif  /* ! __MINGW32__ */

/* Might be called back from NovaWin_StartExecService */
void StartServer(EvalContext *ctx, Policy *policy, GenericAgentConfig *config, ExecdConfig **execd_config, ExecConfig **exec_config)
{
    pthread_attr_init(&threads_attrs);
    pthread_attr_setdetachstate(&threads_attrs, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&threads_attrs, (size_t)2048*1024);

    Banner("Starting executor");

#ifndef __MINGW32__
    if (!ONCE)
    {
        /* Kill previous instances of cf-execd if those are still running */
        Apoptosis();
    }

    time_t now = time(NULL);
    if ((!NO_FORK) && (fork() != 0))
    {
        Log(LOG_LEVEL_INFO, "cf-execd starting %.24s", ctime(&now));
        _exit(EXIT_SUCCESS);
    }

    if (!NO_FORK)
    {
        ActAsDaemon();
    }

#else  /* __MINGW32__ */

    if (!NO_FORK)
    {
        Log(LOG_LEVEL_VERBOSE, "Windows does not support starting processes in the background - starting in foreground");
    }

#endif

    WritePID("cf-execd.pid");
    signal(SIGINT, HandleSignalsForDaemon);
    signal(SIGTERM, HandleSignalsForDaemon);
    signal(SIGBUS, HandleSignalsForDaemon);
    signal(SIGHUP, HandleSignalsForDaemon);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGUSR1, HandleSignalsForDaemon);
    signal(SIGUSR2, HandleSignalsForDaemon);

    umask(077);

    int runagent_socket = -1;

#ifndef __MINGW32__
    if (UsingRunagentSocket())
    {
        runagent_socket = SetupRunagentSocket(*execd_config);
    }
#endif

    if (ONCE)
    {
        LocalExec(*exec_config);
        CloseLog();
    }
    else
    {
        CFExecdMainLoop(ctx, &policy, config, execd_config, exec_config, runagent_socket);
    }
    PolicyDestroy(policy);
}

/*****************************************************************************/

#ifndef __MINGW32__
static pid_t LocalExecInFork(const ExecConfig *config)
{
    Log(LOG_LEVEL_VERBOSE, "Forking for exec_command execution");

    pid_t pid = fork();
    if (pid == -1)
    {
        Log(LOG_LEVEL_ERR, "Failed to fork for exec_command execution: %s",
            GetErrorStr());
        return -1;
    }
    else if (pid == 0)
    {
        /* child */
        LocalExec(config);
        Log(LOG_LEVEL_VERBOSE, "Finished exec_command execution, terminating the forked process");
        _exit(0);
    }
    else
    {
        /* parent */
        return pid;
    }
}

#else
static void *LocalExecThread(void *param)
{
    ExecConfig *config = (ExecConfig *) param;
    LocalExec(config);
    ExecConfigDestroy(config);

    Log(LOG_LEVEL_VERBOSE, "Finished exec_command execution, terminating thread");
    return NULL;
}

static bool LocalExecInThread(const ExecConfig *config)
{
    ExecConfig *thread_config = ExecConfigCopy(config);
    pthread_t tid;

    Log(LOG_LEVEL_VERBOSE, "Spawning thread for exec_command execution");
    int ret = pthread_create(&tid, &threads_attrs, LocalExecThread, thread_config);
    if (ret != 0)
    {
        ExecConfigDestroy(thread_config);
        Log(LOG_LEVEL_ERR, "Failed to create thread (pthread_create: %s)",
            GetErrorStr());
        return false;
    }

    return true;
}
#endif  /* ! __MINGW32__ */

#ifndef __MINGW32__

static void Apoptosis(void)
{
    char promiser_buf[CF_SMALLBUF];
    snprintf(promiser_buf, sizeof(promiser_buf), "%s%ccf-execd",
             GetBinDir(), FILE_SEPARATOR);

    if (LoadProcessTable())
    {
        char myuid[PRINTSIZE(unsigned)];
        xsnprintf(myuid, sizeof(myuid), "%u", (unsigned) getuid());

        Rlist *owners = NULL;
        RlistPrepend(&owners, myuid, RVAL_TYPE_SCALAR);

        ProcessSelect process_select = PROCESS_SELECT_INIT;
        process_select.owner = owners;
        process_select.process_result = "process_owner";

        Item *killlist = SelectProcesses(promiser_buf, &(process_select), true);
        RlistDestroy(owners);

        for (Item *ip = killlist; ip != NULL; ip = ip->next)
        {
            pid_t pid = ip->counter;

            if (pid != getpid() && kill(pid, SIGTERM) < 0)
            {
                if (errno == ESRCH)
                {
                    /* That's ok, process exited voluntarily */
                }
                else
                {
                    Log(LOG_LEVEL_ERR, "Unable to kill stale cf-execd process pid=%d. (kill: %s)",
                        (int)pid, GetErrorStr());
                }
            }
        }
    }

    ClearProcessTable();

    Log(LOG_LEVEL_VERBOSE, "Pruning complete");
}

#endif

typedef enum
{
    RELOAD_ENVIRONMENT,
    RELOAD_FULL
} Reload;

static Reload CheckNewPromises(GenericAgentConfig *config)
{
    Log(LOG_LEVEL_DEBUG, "Checking file updates for input file '%s'", config->input_file);

    time_t validated_at = ReadTimestampFromPolicyValidatedFile(config, NULL);

    bool reload_config = false;

    if (config->agent_specific.daemon.last_validated_at < validated_at)
    {
        Log(LOG_LEVEL_VERBOSE, "New promises detected...");
        reload_config = true;
    }
    if (ReloadConfigRequested())
    {
        Log(LOG_LEVEL_VERBOSE, "Force reload of inputs files...");
        reload_config = true;
    }

    if (reload_config)
    {
        ClearRequestReloadConfig();

        /* Rereading policies now, so update timestamp. */
        config->agent_specific.daemon.last_validated_at = validated_at;

        if (GenericAgentArePromisesValid(config))
        {
            return RELOAD_FULL;
        }
        else
        {
            Log(LOG_LEVEL_INFO, "New promises file contains syntax errors -- ignoring");
        }
    }
    else
    {
        Log(LOG_LEVEL_DEBUG, "No new promises found");
    }

    return RELOAD_ENVIRONMENT;
}

static bool ScheduleRun(EvalContext *ctx, Policy **policy, GenericAgentConfig *config,
                        ExecdConfig **execd_config, ExecConfig **exec_config)
{
    /*
     * FIXME: this logic duplicates the one from cf-serverd.c. Unify ASAP.
     */

    if (CheckNewPromises(config) == RELOAD_FULL)
    {
        /* Full reload */

        Log(LOG_LEVEL_INFO, "Re-reading promise file '%s'", config->input_file);

        EvalContextClear(ctx);

        strcpy(VDOMAIN, "undefined.domain");

        PolicyDestroy(*policy);
        *policy = NULL;

        EvalContextSetPolicyServerFromFile(ctx, GetWorkDir());
        UpdateLastPolicyUpdateTime(ctx);

        DetectEnvironment(ctx);
        GenericAgentDiscoverContext(ctx, config, NULL);

        EvalContextClassPutHard(ctx, CF_AGENTTYPES[AGENT_TYPE_EXECUTOR], "cfe_internal,source=agent");

        time_t t = SetReferenceTime();
        UpdateTimeClasses(ctx, t);

        GenericAgentConfigSetBundleSequence(config, NULL);

#ifndef __MINGW32__
        /* Take over the runagent_socket_allow_users set for comparison. */
        StringSet *old_runagent_allow_users = NULL;
        if (UsingRunagentSocket())
        {
            old_runagent_allow_users = (*execd_config)->runagent_allow_users;
            (*execd_config)->runagent_allow_users = NULL;
        }
#endif

        *policy = LoadPolicy(ctx, config);
        ExecConfigDestroy(*exec_config);
        ExecdConfigDestroy(*execd_config);

        *exec_config = ExecConfigNew(!ONCE, ctx, *policy);
        *execd_config = ExecdConfigNew(ctx, *policy);

#ifndef __MINGW32__
        if (UsingRunagentSocket())
        {
            /* Check if the old list and the new one differ. */
            if (!StringSetIsEqual(old_runagent_allow_users,
                                  (*execd_config)->runagent_allow_users))
            {
                struct sockaddr_un sock_info;
                if (GetRunagentSocketInfo(&sock_info))
                {
                    bool success = SetRunagentSocketACLs(sock_info.sun_path,
                                                         (*execd_config)->runagent_allow_users);
                    if (!success)
                    {
                        Log(LOG_LEVEL_ERR,
                            "Failed to allow new runagent_socket_allow_users users access the runagent socket"
                            " (on policy reload)");
                        /* keep going anyway */
                    }
                }
                else
                {
                    Log(LOG_LEVEL_ERR, "Failed to get runagent.socket path");
                }
            }
            StringSetDestroy(old_runagent_allow_users);
        }
#endif

        SetFacility((*execd_config)->log_facility);
    }
    else
    {
        /* Environment reload */

        EvalContextClear(ctx);

        DetectEnvironment(ctx);

        time_t t = SetReferenceTime();
        UpdateTimeClasses(ctx, t);
    }

    {
        StringSetIterator it = StringSetIteratorInit((*execd_config)->schedule);
        const char *time_context = NULL;
        while ((time_context = StringSetIteratorNext(&it)))
        {
            if (IsDefinedClass(ctx, time_context))
            {
                Log(LOG_LEVEL_VERBOSE, "Waking up the agent at %s ~ %s", ctime(&CFSTARTTIME), time_context);
                return true;
            }
        }
    }

    Log(LOG_LEVEL_VERBOSE, "Nothing to do at %s", ctime(&CFSTARTTIME));
    return false;
}
