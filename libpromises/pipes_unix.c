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

#include <pipes.h>

#include <mutex.h>
#include <global_mutex.h>
#include <exec_tools.h>
#include <rlist.h>
#include <policy.h>
#include <eval_context.h>
#include <file_lib.h>
#include <signals.h>
#include <string_lib.h>

static bool CfSetuid(uid_t uid, gid_t gid);

static int cf_pwait(pid_t pid);

static pid_t *CHILDREN = NULL; /* GLOBAL_X */
static int MAX_FD = 2048; /* GLOBAL_X */ /* Max number of simultaneous pipes */


static void ChildrenFDInit()
{
    ThreadLock(cft_count);
    if (CHILDREN == NULL)       /* first time */
    {
        CHILDREN = xcalloc(MAX_FD, sizeof(pid_t));
    }

    ThreadUnlock(cft_count);
}

/*****************************************************************************/

/* This leaks memory and is not thread-safe! To be used only when you are
 * about to exec() or _exit(), and only async-signal-safe code is allowed. */
static void ChildrenFDUnsafeClose()
{
    /* GenericCreatePipeAndFork() must have been called to init this. */
    assert(CHILDREN != NULL);

    for (int i = 0; i < MAX_FD; i++)
    {
        if (CHILDREN[i] > 0)
        {
            close(i);
        }
    }
    CHILDREN = NULL;                                    /* leaks on purpose */
}

/* This is the original safe version, but not signal-handler-safe.
   It's currently unused. */
#if 0
static void ChildrenFDClose()
{
    ThreadLock(cft_count);
    int i;
    for (i = 0; i < MAX_FD; i++)
    {
        if (CHILDREN[i] > 0)
        {
            close(i);
        }
    }
    free(CHILDREN);
    CHILDREN = NULL;
    ThreadUnlock(cft_count);
}
#endif

static void ChildOutputSelectDupClose(int pd[2], OutputSelect output_select)
{
    close(pd[0]); // Don't need output from parent

    if (pd[1] != 1) // TODO: When is pd[1] == 1 ???
    {
        if ((output_select == OUTPUT_SELECT_BOTH)
            || (output_select == OUTPUT_SELECT_STDOUT))
        {
            // close our(child) stdout(1) and open (pd[1]) as stdout
            dup2(pd[1], 1);
            // Subsequent stdout output will go to parent (pd[1])
        }
        else
        {
            // Close / discard stdout
            int nullfd = open(NULLFILE, O_WRONLY);
            dup2(nullfd, 1);
            close(nullfd);
        }

        if ((output_select == OUTPUT_SELECT_BOTH)
            || (output_select == OUTPUT_SELECT_STDERR))
        {
            // close our(child) stderr(2) and open (pd[1]) as stderr
            dup2(pd[1], 2);
            // Subsequent stderr output will go to parent (pd[1])
        }
        else
        {
            // Close / discard stderr
            int nullfd = open(NULLFILE, O_WRONLY);
            dup2(nullfd, 2);
            close(nullfd);
        }

        close(pd[1]);
    }
}

/*****************************************************************************/

static void ChildrenFDSet(int fd, pid_t pid)
{
    int new_max = 0;

    if (fd >= MAX_FD)
    {
        Log(LOG_LEVEL_WARNING,
            "File descriptor %d of child %jd higher than MAX_FD, check for defunct children",
            fd, (intmax_t) pid);
        new_max = fd + 32;
    }

    ThreadLock(cft_count);

    if (new_max)
    {
        CHILDREN = xrealloc(CHILDREN, new_max * sizeof(pid_t));
        MAX_FD = new_max;
    }

    CHILDREN[fd] = pid;
    ThreadUnlock(cft_count);
}

/*****************************************************************************/

typedef struct
{
    const char *type;
    int pipe_desc[2];
} IOPipe;

static pid_t GenericCreatePipeAndFork(IOPipe *pipes)
{
    for (int i = 0; i < 2; i++)
    {
        if (pipes[i].type && !PipeTypeIsOk(pipes[i].type))
        {
            errno = EINVAL;
            return -1;
        }
    }

    ChildrenFDInit();

    /* Create pair of descriptors to this process. */
    if (pipes[0].type && pipe(pipes[0].pipe_desc) < 0)
    {
        return -1;
    }

    /* Create second pair of descriptors (if exists) to this process.
     * This will allow full I/O operations. */
    if (pipes[1].type && pipe(pipes[1].pipe_desc) < 0)
    {
        close(pipes[0].pipe_desc[0]);
        close(pipes[0].pipe_desc[1]);
        return -1;
    }

    pid_t pid = -1;

    if ((pid = fork()) == (pid_t) -1)
    {
        /* One pipe will be always here. */
        close(pipes[0].pipe_desc[0]);
        close(pipes[0].pipe_desc[1]);

        /* Second pipe is optional so we have to check existence. */
        if (pipes[1].type)
        {
            close(pipes[1].pipe_desc[0]);
            close(pipes[1].pipe_desc[1]);
        }
        return -1;
    }

    /* Ignore SIGCHLD, by setting the handler to SIG_DFL. NOTE: this is
     * different than setting to SIG_IGN. In the latter case no zombies are
     * generated ever and you can't wait() for the child to finish. */
    struct sigaction sa = {
        .sa_handler = SIG_DFL,
    };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGCHLD, &sa, NULL);

    if (pid == 0)                                               /* child */
    {
        /* WARNING only call async-signal-safe functions in child. */

        /* The fork()ed child is always single-threaded, but we are only
         * allowed to call async-signal-safe functions (man 3p fork). */

        // Redmine #2971: reset SIGPIPE signal handler in the child to have a
        // sane behavior of piped commands within child
        signal(SIGPIPE, SIG_DFL);

        /* The child should always accept all signals after it has exec'd,
         * else the child might be unkillable! (happened in ENT-3147). */
        sigset_t sigmask;
        sigemptyset(&sigmask);
        sigprocmask(SIG_SETMASK, &sigmask, NULL);
    }

    ALARM_PID = (pid != 0 ? pid : -1);

    return pid;
}

/*****************************************************************************/

static pid_t CreatePipeAndFork(const char *type, int *pd)
{
    IOPipe pipes[2];
    pipes[0].type = type;
    pipes[1].type = NULL; /* We don't want to create this one. */

    pid_t pid = GenericCreatePipeAndFork(pipes);

    pd[0] = pipes[0].pipe_desc[0];
    pd[1] = pipes[0].pipe_desc[1];

    return pid;
}

/*****************************************************************************/

static pid_t CreatePipesAndFork(const char *type, int *pd, int *pdb)
{
    IOPipe pipes[2];
    /* Both pipes MUST have the same type. */
    pipes[0].type = type;
    pipes[1].type = type;

    pid_t pid =  GenericCreatePipeAndFork(pipes);

    pd[0] = pipes[0].pipe_desc[0];
    pd[1] =  pipes[0].pipe_desc[1];
    pdb[0] = pipes[1].pipe_desc[0];
    pdb[1] = pipes[1].pipe_desc[1];
    return pid;
}

/*****************************************************************************/

IOData cf_popen_full_duplex(const char *command, bool capture_stderr, bool require_full_path)
{
    /* For simplifying reading and writing directions */
    const int READ=0, WRITE=1;
    int child_pipe[2];  /* From child to parent */
    int parent_pipe[2]; /* From parent to child */
    pid_t pid;

    char **argv = ArgSplitCommand(command);

    fflush(NULL); /* Empty file buffers */
    pid = CreatePipesAndFork("r+t", child_pipe, parent_pipe);

    if (pid == (pid_t) -1)
    {
        Log(LOG_LEVEL_ERR, "Couldn't fork child process: %s", GetErrorStr());
        ArgFree(argv);
        return (IOData) {-1, -1, NULL, NULL};
    }

    else if (pid > 0) // parent
    {
        close(child_pipe[WRITE]);
        close(parent_pipe[READ]);

        IOData io_desc;
        io_desc.write_fd = parent_pipe[WRITE];
        io_desc.read_fd = child_pipe[READ];
        io_desc.read_stream = NULL;
        io_desc.write_stream = NULL;

        ChildrenFDSet(parent_pipe[WRITE], pid);
        ChildrenFDSet(child_pipe[READ], pid);
        ArgFree(argv);
        return io_desc;
    }
    else // child
    {
        close(child_pipe[READ]);
        close(parent_pipe[WRITE]);

        /* Open stdin from parant process and stdout from child */
        if (dup2(parent_pipe[READ], 0) == -1 || dup2(child_pipe[WRITE],1) == -1)
        {
            Log(LOG_LEVEL_ERR, "Can not execute dup2: %s", GetErrorStr());
            _exit(EXIT_FAILURE);
        }

        if (capture_stderr)
        {
            /* Merge stdout/stderr */
            if(dup2(child_pipe[WRITE], 2) == -1)
            {
                Log(LOG_LEVEL_ERR, "Can not execute dup2 for merging stderr: %s",
                    GetErrorStr());
                _exit(EXIT_FAILURE);
            }
        }
        else
        {
            /* leave stderr open */
        }

        close(child_pipe[WRITE]);
        close(parent_pipe[READ]);

        ChildrenFDUnsafeClose();

        int res;
        if (require_full_path)
        {
            res = execv(argv[0], argv);
        }
        else
        {
            res = execvp(argv[0], argv);
        }

        if (res == -1)
        {
            /* NOTE: exec functions return only when error have occurred. */
            Log(LOG_LEVEL_ERR, "Couldn't run '%s'. (%s: %s)",
                argv[0],
                require_full_path ? "execv" : "execvp",
                GetErrorStr());
        }

        /* We shouldn't reach this point */
        _exit(EXIT_FAILURE);
    }
}

FILE *cf_popen_select(const char *command, const char *type, OutputSelect output_select)
{
    int pd[2];
    pid_t pid;
    FILE *pp = NULL;

    char **argv = ArgSplitCommand(command);

    pid = CreatePipeAndFork(type, pd);
    if (pid == (pid_t) -1)
    {
        ArgFree(argv);
        return NULL;
    }

    if (pid == 0)                                               /* child */
    {
        /* WARNING only call async-signal-safe functions in child. */

        switch (*type)
        {
        case 'r':
            ChildOutputSelectDupClose(pd, output_select);
            break;

        case 'w':

            close(pd[1]);

            if (pd[0] != 0)
            {
                dup2(pd[0], 0);
                close(pd[0]);
            }
        }

        ChildrenFDUnsafeClose();

        if (execv(argv[0], argv) == -1)
        {
            Log(LOG_LEVEL_ERR, "Couldn't run '%s'. (execv: %s)", argv[0], GetErrorStr());
        }

        _exit(EXIT_FAILURE);
    }
    else                                                        /* parent */
    {
        switch (*type)
        {
        case 'r':

            close(pd[1]);

            if ((pp = fdopen(pd[0], type)) == NULL)
            {
                cf_pwait(pid);
                ArgFree(argv);
                return NULL;
            }
            break;

        case 'w':

            close(pd[0]);

            if ((pp = fdopen(pd[1], type)) == NULL)
            {
                cf_pwait(pid);
                ArgFree(argv);
                return NULL;
            }
        }

        ChildrenFDSet(fileno(pp), pid);
        ArgFree(argv);
        return pp;
    }

    ProgrammingError("Unreachable code");
    return NULL;
}

FILE *cf_popen(const char *command, const char *type, bool capture_stderr)
{
    return cf_popen_select(
        command,
        type,
        capture_stderr ? OUTPUT_SELECT_BOTH : OUTPUT_SELECT_STDOUT);
}

/*****************************************************************************/

/*
 * WARNING: this is only allowed to be called from single-threaded code,
 *          because of the safe_chdir() call in the forked child.
 */
FILE *cf_popensetuid(const char *command, const char *type,
                     uid_t uid, gid_t gid, char *chdirv, char *chrootv,
                     ARG_UNUSED int background)
{
    int pd[2];
    pid_t pid;
    FILE *pp = NULL;

    char **argv = ArgSplitCommand(command);

    pid = CreatePipeAndFork(type, pd);
    if (pid == (pid_t) -1)
    {
        ArgFree(argv);
        return NULL;
    }

    if (pid == 0)                                               /* child */
    {
        /* WARNING only call async-signal-safe functions in child. */

        switch (*type)
        {
        case 'r':

            close(pd[0]);       /* Don't need output from parent */

            if (pd[1] != 1)
            {
                dup2(pd[1], 1); /* Attach pp=pd[1] to our stdout */
                dup2(pd[1], 2); /* Merge stdout/stderr */
                close(pd[1]);
            }

            break;

        case 'w':

            close(pd[1]);

            if (pd[0] != 0)
            {
                dup2(pd[0], 0);
                close(pd[0]);
            }
        }

        ChildrenFDUnsafeClose();

        if (chrootv && (strlen(chrootv) != 0))
        {
            if (chroot(chrootv) == -1)
            {
                Log(LOG_LEVEL_ERR, "Couldn't chroot to '%s'. (chroot: %s)", chrootv, GetErrorStr());
                _exit(EXIT_FAILURE);
            }
        }

        if (chdirv && (strlen(chdirv) != 0))
        {
            if (safe_chdir(chdirv) == -1)
            {
                Log(LOG_LEVEL_ERR, "Couldn't chdir to '%s'. (chdir: %s)", chdirv, GetErrorStr());
                _exit(EXIT_FAILURE);
            }
        }

        if (!CfSetuid(uid, gid))
        {
            _exit(EXIT_FAILURE);
        }

        if (execv(argv[0], argv) == -1)
        {
            Log(LOG_LEVEL_ERR, "Couldn't run '%s'. (execv: %s)", argv[0], GetErrorStr());
        }

        _exit(EXIT_FAILURE);
    }
    else                                                        /* parent */
    {
        switch (*type)
        {
        case 'r':

            close(pd[1]);

            if ((pp = fdopen(pd[0], type)) == NULL)
            {
                cf_pwait(pid);
                ArgFree(argv);
                return NULL;
            }
            break;

        case 'w':

            close(pd[0]);

            if ((pp = fdopen(pd[1], type)) == NULL)
            {
                cf_pwait(pid);
                ArgFree(argv);
                return NULL;
            }
        }

        ChildrenFDSet(fileno(pp), pid);
        ArgFree(argv);
        return pp;
    }

    ProgrammingError("Unreachable code");
    return NULL;
}

/*****************************************************************************/
/* Shell versions of commands - not recommended for security reasons         */
/*****************************************************************************/

FILE *cf_popen_sh_select(const char *command, const char *type, OutputSelect output_select)
{
    int pd[2];
    pid_t pid;
    FILE *pp = NULL;

    pid = CreatePipeAndFork(type, pd);
    if (pid == (pid_t) -1)
    {
        return NULL;
    }

    if (pid == 0)                                               /* child */
    {
        /* WARNING only call async-signal-safe functions in child. */

        switch (*type)
        {
        case 'r':
            ChildOutputSelectDupClose(pd, output_select);

            break;

        case 'w':

            close(pd[1]);

            if (pd[0] != 0)
            {
                dup2(pd[0], 0);
                close(pd[0]);
            }
        }

        ChildrenFDUnsafeClose();

        execl(SHELL_PATH, "sh", "-c", command, NULL);

        Log(LOG_LEVEL_ERR, "Couldn't run: '%s'  (execl: %s)", command, GetErrorStr());
        _exit(EXIT_FAILURE);
    }
    else                                                        /* parent */
    {
        switch (*type)
        {
        case 'r':

            close(pd[1]);

            if ((pp = fdopen(pd[0], type)) == NULL)
            {
                cf_pwait(pid);
                return NULL;
            }
            break;

        case 'w':

            close(pd[0]);

            if ((pp = fdopen(pd[1], type)) == NULL)
            {
                cf_pwait(pid);
                return NULL;
            }
        }

        ChildrenFDSet(fileno(pp), pid);
        return pp;
    }

    ProgrammingError("Unreachable code");
    return NULL;
}

FILE *cf_popen_sh(const char *command, const char *type)
{
    return cf_popen_sh_select(command, type, OUTPUT_SELECT_BOTH);
}

/******************************************************************************/

/*
 * WARNING: this is only allowed to be called from single-threaded code,
 *          because of the safe_chdir() call in the forked child.
 */
FILE *cf_popen_shsetuid(const char *command, const char *type,
                        uid_t uid, gid_t gid, char *chdirv, char *chrootv,
                        ARG_UNUSED int background)
{
    int pd[2];
    pid_t pid;
    FILE *pp = NULL;

    pid = CreatePipeAndFork(type, pd);
    if (pid == (pid_t) -1)
    {
        return NULL;
    }

    if (pid == 0)                                               /* child */
    {
        /* WARNING only call async-signal-safe functions in child. */

        switch (*type)
        {
        case 'r':

            close(pd[0]);       /* Don't need output from parent */

            if (pd[1] != 1)
            {
                dup2(pd[1], 1); /* Attach pp=pd[1] to our stdout */
                dup2(pd[1], 2); /* Merge stdout/stderr */
                close(pd[1]);
            }

            break;

        case 'w':

            close(pd[1]);

            if (pd[0] != 0)
            {
                dup2(pd[0], 0);
                close(pd[0]);
            }
        }

        ChildrenFDUnsafeClose();

        if (chrootv && (strlen(chrootv) != 0))
        {
            if (chroot(chrootv) == -1)
            {
                Log(LOG_LEVEL_ERR, "Couldn't chroot to '%s'. (chroot: %s)", chrootv, GetErrorStr());
                _exit(EXIT_FAILURE);
            }
        }

        if (chdirv && (strlen(chdirv) != 0))
        {
            if (safe_chdir(chdirv) == -1)
            {
                Log(LOG_LEVEL_ERR, "Couldn't chdir to '%s'. (chdir: %s)", chdirv, GetErrorStr());
                _exit(EXIT_FAILURE);
            }
        }

        if (!CfSetuid(uid, gid))
        {
            _exit(EXIT_FAILURE);
        }

        execl(SHELL_PATH, "sh", "-c", command, NULL);

        Log(LOG_LEVEL_ERR, "Couldn't run: '%s'  (execl: %s)", command, GetErrorStr());
        _exit(EXIT_FAILURE);
    }
    else                                                        /* parent */
    {
        switch (*type)
        {
        case 'r':

            close(pd[1]);

            if ((pp = fdopen(pd[0], type)) == NULL)
            {
                cf_pwait(pid);
                return NULL;
            }
            break;

        case 'w':

            close(pd[0]);

            if ((pp = fdopen(pd[1], type)) == NULL)
            {
                cf_pwait(pid);
                return NULL;
            }
        }

        ChildrenFDSet(fileno(pp), pid);
        return pp;
    }

    ProgrammingError("Unreachable code");
    return NULL;
}

static int cf_pwait(pid_t pid)
{
    Log(LOG_LEVEL_DEBUG,
        "cf_pwait - waiting for process %jd", (intmax_t) pid);

    int status;
    while (waitpid(pid, &status, 0) < 0)
    {
        if (errno != EINTR)
        {
            Log(LOG_LEVEL_ERR,
                "Waiting for child PID %jd failed (waitpid: %s)",
                (intmax_t) pid, GetErrorStr());
            return -1;
        }
    }

    if (!WIFEXITED(status))
    {
        Log(LOG_LEVEL_VERBOSE,
            "Child PID %jd exited abnormally (%s)", (intmax_t) pid,
            WIFSIGNALED(status) ? "signalled" : (
                WIFSTOPPED(status) ? "stopped" : (
                    WIFCONTINUED(status) ? "continued" : "unknown" )));
        return -1;
    }

    int retcode = WEXITSTATUS(status);

    Log(LOG_LEVEL_DEBUG, "cf_pwait - process %jd exited with code: %d",
        (intmax_t) pid, retcode);
    return retcode;
}

/*******************************************************************/

/**
 * Closes the pipe without waiting the child.
 *
 * @param pp pipe to the child process
 */
void cf_pclose_nowait(FILE *pp)
{
    if (fclose(pp) == EOF)
    {
        Log(LOG_LEVEL_ERR,
            "Could not close the pipe to the executed subcommand (fclose: %s)",
            GetErrorStr());
    }
}

/**
 * Closes the pipe and wait()s for PID of the child,
 * in order to reap the zombies.
 */
int cf_pclose(FILE *pp)
{
    int fd = fileno(pp);
    pid_t pid;

    ThreadLock(cft_count);

    if (CHILDREN == NULL)       /* popen hasn't been called */
    {
        ThreadUnlock(cft_count);
        fclose(pp);
        return -1;
    }

    ALARM_PID = -1;

    if (fd >= MAX_FD)
    {
        ThreadUnlock(cft_count);
        Log(LOG_LEVEL_ERR,
            "File descriptor %d of child higher than MAX_FD in cf_pclose!",
            fd);
        fclose(pp);
        return -1;
    }

    pid = CHILDREN[fd];
    CHILDREN[fd] = 0;
    ThreadUnlock(cft_count);

    if (fclose(pp) == EOF)
    {
        Log(LOG_LEVEL_ERR,
            "Could not close the pipe to the executed subcommand (fclose: %s)",
            GetErrorStr());
    }

    return cf_pwait(pid);
}

int cf_pclose_full_duplex_side(int fd)
{
    ThreadLock(cft_count);

    if (CHILDREN == NULL)       /* popen hasn't been called */
    {
        ThreadUnlock(cft_count);
        close(fd);
        return -1;
    }

    if (fd >= MAX_FD)
    {
        ThreadUnlock(cft_count);
        Log(LOG_LEVEL_ERR,
            "File descriptor %d of child higher than MAX_FD in cf_pclose_full_duplex_side!",
            fd);
    }
    else
    {
        CHILDREN[fd] = 0;
        ThreadUnlock(cft_count);
    }
    return close(fd);
}


/* We are assuming that read_fd part will be always open at this point. */
int cf_pclose_full_duplex(IOData *data)
{
    assert(data != NULL);
    ThreadLock(cft_count);

    if (CHILDREN == NULL)
    {
        ThreadUnlock(cft_count);
        if (data->read_stream != NULL)
        {
            // fclose closes the underlying fd / socket,
            // but we need the fd for handling of processes below.
            assert(data->read_fd >= 0);
            fclose(data->read_stream);
        }
        else if (data->read_fd >= 0)
        {
            close(data->read_fd);
        }

        if (data->write_stream != NULL)
        {
            // fclose closes the underlying fd / socket,
            // but we need the fd for handling of processes below.
            assert(data->write_fd >= 0);
            fclose(data->write_stream);
        }
        else if (data->write_fd >= 0)
        {
            close(data->write_fd);
        }
        return -1;
    }

    ALARM_PID = -1;
    pid_t pid = 0;

    /* Safe as pipes[1] is -1 if not initialized */
    if (data->read_fd >= MAX_FD || data->write_fd >= MAX_FD)
    {
        ThreadUnlock(cft_count);
        Log(LOG_LEVEL_ERR,
            "File descriptor %d of child higher than MAX_FD in cf_pclose!",
            data->read_fd > data->write_fd ? data->read_fd : data->write_fd);
    }
    else
    {
        pid = CHILDREN[data->read_fd];
        if (data->write_fd >= 0)
        {
            assert(pid == CHILDREN[data->write_fd]);
            CHILDREN[data->write_fd] = 0;
        }
        CHILDREN[data->read_fd] = 0;
        ThreadUnlock(cft_count);
    }

    if (data->read_stream != NULL)
    {
        // Stream is open, fclose it
        if (fclose(data->read_stream) != 0)
        {
            return -1;
        }
    }
    else
    {
        // No stream, just close fd
        if (close(data->read_fd) != 0)
        {
            return -1;
        }
    }

    if (data->write_fd >= 0)
    {
        // write fd needs to be closed
        if (data->write_stream != NULL)
        {
            // Stream is open, fclose it
            if (fclose(data->write_stream) != 0)
            {
                return -1;
            }
        }
        else
        {
            // No stream, close fd directly
            if (close(data->write_fd) != 0)
            {
                return -1;
            }
        }
    }

    if (pid == 0)
    {
        return -1;
    }

    return cf_pwait(pid);
}

bool PipeToPid(pid_t *pid, FILE *pp)
{
    int fd = fileno(pp);
    ThreadLock(cft_count);

    if (CHILDREN == NULL)       /* popen hasn't been called */
    {
        ThreadUnlock(cft_count);
        return false;
    }

    *pid = CHILDREN[fd];
    ThreadUnlock(cft_count);

    return true;
}

/*******************************************************************/

static bool CfSetuid(uid_t uid, gid_t gid)
{
    struct passwd *pw;

    if (gid != (gid_t) - 1)
    {
        Log(LOG_LEVEL_VERBOSE, "Changing gid to %ju", (uintmax_t)gid);

        if (setgid(gid) == -1)
        {
            Log(LOG_LEVEL_ERR, "Couldn't set gid to '%ju'. (setgid: %s)", (uintmax_t)gid, GetErrorStr());
            return false;
        }

        /* Now eliminate any residual privileged groups */

        if ((pw = getpwuid(uid)) == NULL)
        {
            Log(LOG_LEVEL_ERR, "Unable to get login groups when dropping privilege to '%ju'. (getpwuid: %s)", (uintmax_t)uid, GetErrorStr());
            return false;
        }

        if (initgroups(pw->pw_name, pw->pw_gid) == -1)
        {
            Log(LOG_LEVEL_ERR, "Unable to set login groups when dropping privilege to '%s=%ju'. (initgroups: %s)", pw->pw_name,
                  (uintmax_t)uid, GetErrorStr());
            return false;
        }
    }

    if (uid != (uid_t) - 1)
    {
        Log(LOG_LEVEL_VERBOSE, "Changing uid to '%ju'", (uintmax_t)uid);

        if (setuid(uid) == -1)
        {
            Log(LOG_LEVEL_ERR, "Couldn't set uid to '%ju'. (setuid: %s)", (uintmax_t)uid, GetErrorStr());
            return false;
        }
    }

    return true;
}

/* For Windows we need a different method because select() does not */
/* work with non-socket file descriptors. */
int PipeIsReadWriteReady(const IOData *io, int timeout_sec)
{
    fd_set  rset;
    FD_ZERO(&rset);
    FD_SET(io->read_fd, &rset);

    struct timeval tv = {
        .tv_sec = timeout_sec,
        .tv_usec = 0,
    };

    Log(LOG_LEVEL_DEBUG,
        "PipeIsReadWriteReady: wait max %ds for data on fd %d",
        timeout_sec, io->read_fd);

    int ret = select(io->read_fd + 1, &rset, NULL, NULL, &tv);

    if (ret < 0)
    {
        Log(LOG_LEVEL_VERBOSE, "Failed checking for data (select: %s)",
            GetErrorStr());
        return -1;
    }
    else if (FD_ISSET(io->read_fd, &rset))
    {
        return io->read_fd;
    }
    else if (ret == 0)
    {
        /* timeout_sec has elapsed but no data was available. */
        return 0;
    }
    else
    {
        UnexpectedError("select() returned > 0 but our only fd is not set!");
        return -1;
    }
}
