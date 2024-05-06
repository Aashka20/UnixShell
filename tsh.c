/* 
 * tsh - A tiny shell program with job control
 * 
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>

/* Misc manifest constants */
#define MAXLINE    1024   /* max line size */
#define MAXARGS     128   /* max args on a command line */
#define MAXJOBS      16   /* max jobs at any point in time */

/* Job states */
#define UNDEF 0 /* undefined */
#define FG 1    /* running in foreground */
#define BG 2    /* running in background */
#define ST 3    /* stopped */

/* 
 * Jobs states: FG (foreground), BG (background), ST (stopped)
 * Job state transitions and enabling actions:
 *     FG -> ST  : ctrl-z
 *     ST -> FG  : fg command
 *     ST -> BG  : bg command
 *     BG -> FG  : fg command
 * At most 1 job can be in the FG state.
 */

/* Global variables */
extern char **environ;      /* defined in libc */
char prompt[] = "tsh> ";    /* command line prompt (DO NOT CHANGE) */
int verbose = 0;            /* if true, print additional output */
char sbuf[MAXLINE];         /* for composing sprintf messages */

struct job_t {              /* Per-job data */
    pid_t pid;              /* job PID */
    int jid;                /* job ID [1, 2, ...] */
    int state;              /* UNDEF, FG, BG, or ST */
    char cmdline[MAXLINE];  /* command line */
};
struct job_t jobs[MAXJOBS]; /* The job list */

volatile sig_atomic_t ready; /* Is the newest child in its own process group? */

/* End global variables */


/* Function prototypes */

/* Here are the functions that you will implement */
void eval(char *cmdline);
int builtin_cmd(char **argv);
void do_bgfg(char **argv);
void waitfg(pid_t pid);
void sigchld_handler(int sig);
void sigint_handler(int sig);
void sigtstp_handler(int sig);

/* Here are helper routines that we've provided for you */
int parseline(const char *cmdline, char **argv); 
void sigquit_handler(int sig);
void sigusr1_handler(int sig);

void clearjob(struct job_t *job);
void initjobs(struct job_t *jobs);
int freejid(struct job_t *jobs); 
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline);
int deletejob(struct job_t *jobs, pid_t pid); 
pid_t fgpid(struct job_t *jobs);
struct job_t *getjobpid(struct job_t *jobs, pid_t pid);
struct job_t *getjobjid(struct job_t *jobs, int jid); 
int pid2jid(pid_t pid); 
void listjobs(struct job_t *jobs);

void usage(void);
void unix_error(char *msg);
void app_error(char *msg);
typedef void handler_t(int);
handler_t *Signal(int signum, handler_t *handler);

/*
 * main - The shell's main routine 
 */
int main(int argc, char **argv) {
    int c;
    char cmdline[MAXLINE];
    int emit_prompt = 1; /* emit prompt (default) */

    /* Redirect stderr to stdout (so that driver will get all output
     * on the pipe connected to stdout) */
    dup2(STDOUT_FILENO, STDERR_FILENO);

    /* Parse the command line */
    while ((c = getopt(argc, argv, "hvp")) != -1) {
        switch (c) {
            case 'h':             /* print help message */
                usage();
                break;
            case 'v':             /* emit additional diagnostic info */
                verbose = 1;
                break;
            case 'p':             /* don't print a prompt */
                emit_prompt = 0;  /* handy for automatic testing */
                break;
            default:
                // usage();
        }
    }

    /* Install the signal handlers */

    Signal(SIGUSR1, sigusr1_handler); /* Child is ready */

    /* These are the ones you will need to implement */
    Signal(SIGINT,  sigint_handler);   /* ctrl-c */
    Signal(SIGTSTP, sigtstp_handler);  /* ctrl-z */
    Signal(SIGCHLD, sigchld_handler);  /* Terminated or stopped child */

    /* This one provides a clean way to kill the shell */
    Signal(SIGQUIT, sigquit_handler); 

    /* Initialize the job list */
    initjobs(jobs);

    /* Execute the shell's read/eval loop */
    while (1) {

        /* Read command line */
        if (emit_prompt) {
            printf("%s", prompt);
            fflush(stdout);
        }
        if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
            app_error("fgets error");
        if (feof(stdin)) { /* End of file (ctrl-d) */
            fflush(stdout);
            exit(0);
        }

        /* Evaluate the command line */
        eval(cmdline);
        fflush(stdout);
    } 

    exit(0); /* control never reaches here */
}

void redirect_stream(int old, int new) {
    if(old == -1) {
        unix_error("open");
        exit(-1);
    }

    if(dup2(old, new) == -1) {
        unix_error("dup2");
        exit(-1);
    }
}

/*
 * Remove num_args elements from the array
 * Shift all following elements into the new spots
 * Put a 0x0 at the end
 */
void remove_args(char **arr, int arr_l, int index, int num_args) {
    for(int i = index; i < arr_l; i++) {
        arr[i] = arr[i + num_args];
    }
    arr[index + num_args] = 0x0;
    return;
}

/*
 * Check for pipelined commands
 * If a pipeline is found, stick the first program's output to the second
 * program's input
 */
int check_pipeline(int argc, char **argv) {
    for(int i = 0; i < argc; i++) {
        if(strcmp(argv[i], "|") == 0) { // Pipeline detected
            // Pipe then fork
            int fds[2];
            if(pipe(fds) < 0) {
                unix_error("pipe");
                exit(-1);
            }
            int pid = fork();
            if(pid < 0) {
                unix_error("fork");
                exit(-1);
            }
            // Child continues
            if(pid == 0) {
                // Connect child output to parent input
                close(fds[0]);
                redirect_stream(fds[1], 1);

                // Child needs to remove everything before the pipeline and
                // continue in the for loop
                argc -= i + 1;
                memmove(argv, argv + i + 1, argc * sizeof(char *));
                argv[argc] = 0x0;
                i = 0;
            }
            if(pid > 0) {
                // Parent returns everything before the pipeline
                close(fds[1]);
                redirect_stream(fds[0], 0);
                argv[i] = 0x0;
                wait(NULL);
                return argc;
            }
        }
    }
    return argc;
}

/* 
 * eval - Evaluate the command line that the user has just typed in
 * 
 * If the user has requested a built-in command (quit, jobs, bg or fg)
 * then execute it immediately. Otherwise, fork a child process and
 * run the job in the context of the child. If the job is running in
 * the foreground, wait for it to terminate and then return.  Note:
 * each child process must have a unique process group ID so that our
 * background children don't receive SIGINT (SIGTSTP) from the kernel
 * when we type ctrl-c (ctrl-z) at the keyboard.  
*/
void eval(char *cmdline) {
    char *argv[MAXARGS];
    int argc = parseline(cmdline, argv); // Populates argv
    if(argc == 0) { return; }

    if(builtin_cmd(argv) != 0) { // Return if builtin command
        return;
    }

    int state = FG;
    if(strcmp(argv[argc - 1], "&") == 0) { // Background job
        state = BG;
    }

    // Block signals before adding job
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGCHLD);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTSTP);
    if(sigprocmask(SIG_BLOCK, &set, NULL) < 0) {
        unix_error("sigprocmask");
        exit(-1);
    }

    signal(SIGINT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);

    int f = fork();
    if(f < 0) {
        unix_error("fork");
        exit(-1);
    }
    if(f == 0) { // Child
        // Pipelining
        argc = check_pipeline(argc, argv);

        // Redirect streams
        int fd;
        for(int i = 0; i < argc - 1; i++) {
            char *arg = argv[i];
            if(strcmp(arg, "<") == 0) {
                fd = open(argv[i + 1], O_RDONLY);
                redirect_stream(fd, 0);

                remove_args(argv, argc, i, 2);
                argc -= 2;
            }
            if(strcmp(arg, ">") == 0) {
                fd = open(argv[i + 1], O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
                redirect_stream(fd, 1);
                redirect_stream(fd, 2);

                remove_args(argv, argc, i, 2);
                argc -= 2;
            }
        }

        // Messing with signals
        setpgid(0, 0);
        signal(SIGINT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        sigprocmask(SIG_UNBLOCK, &set, NULL);

        // execve with command args
        char *pathname = argv[0];
        if(state == BG) {
            argv[argc - 1] = 0x0;
        }

        fflush(stdin);
        execve(pathname, argv, NULL);
        fprintf(stderr, "tsh: %s", pathname);
        unix_error("");
        exit(-1);
    }
    else { // Parent
        // Wait for child to terminate then return
        if(addjob(jobs, f, state, cmdline) != 1) {
            unix_error("addjob");
            exit(-1);
        }

        sigprocmask(SIG_UNBLOCK, &set, NULL);

        signal(SIGINT, sigint_handler); // Alan
        signal(SIGTSTP, sigtstp_handler);
        signal(SIGCHLD, sigchld_handler);

        // If foreground, wait for termination
        if(state == BG) {
            struct job_t job = *getjobpid(jobs, f);
            printf("[%d] (%d) ", job.jid, job.pid);
            printf("Running ");
            printf("%s", job.cmdline);
            return;
        }
        waitfg(f);
    }

    return;
}

/*
 * parseline - Parse the command line and build the argv array.
 * 
 * Characters enclosed in single quotes are treated as a single
 * argument.  Return number of arguments parsed.
 */
int parseline(const char *cmdline, char **argv) {
    static char array[MAXLINE]; /* holds local copy of command line */
    char *buf = array;          /* ptr that traverses command line */
    char *delim;                /* points to space or quote delimiters */
    int argc;                   /* number of args */

    strcpy(buf, cmdline);
    buf[strlen(buf)-1] = ' ';  /* replace trailing '\n' with space */
    while (*buf && (*buf == ' ')) /* ignore leading spaces */
        buf++;

    /* Build the argv list */
    argc = 0;
    if (*buf == '\'') {
        buf++;
        delim = strchr(buf, '\'');
    }
    else {
        delim = strchr(buf, ' ');
    }

    while (delim) {
        argv[argc++] = buf;
        *delim = '\0';
        buf = delim + 1;
        while (*buf && (*buf == ' ')) /* ignore spaces */
            buf++;

        if (*buf == '\'') {
            buf++;
            delim = strchr(buf, '\'');
        }
        else {
            delim = strchr(buf, ' ');
        }
    }
    argv[argc] = NULL;
    
    return argc;
}

// Aashka
/* 
 * builtin_cmd - If the user has typed a built-in command then execute
 *    it immediately.  
 */
int builtin_cmd(char **argv) {
    // return 1 to indicate successful execution of builtin commands 
    if(strcmp(argv[0], "jobs") == 0) {
        // list all background jobs 
        listjobs(jobs);
        return 1;
    }
    else if (strcmp(argv[0], "quit") == 0) {
        exit(1);
        
    }
    else if ((strcmp(argv[0], "bg") == 0 || strcmp(argv[0],"fg") == 0)) {
        if (argv[1] == NULL) {
            printf("Missing process Id or Job id\n");
        }
        // run the job defined in argv[1] by passing it a SIGCONT signal 
        else{
            do_bgfg(argv);
        }
        return 1;
    }

    return 0;     /* not a builtin command */
}

// Aashka
/* 
 * do_bgfg - Execute the builtin bg and fg commands
 */
void do_bgfg(char **argv) {
    pid_t pid;
    struct job_t *job;

    // check if argv[1] is PID or JID(% prefixed)
    int is_jid = (argv[1][0] == '%');
    if (is_jid){
        //if jid, skip % and conver the rest into int 
        int jid = atoi(argv[1] + 1);
        // get job and pid from jid 
        job = getjobjid(jobs, jid);
        
        if (job == NULL) {
            printf("%s: No such job\n", argv[1]);
            return;
        }
        pid = job->pid;
    }
    else {
        // process id - convert argv[1] into int 
        pid = atoi(argv[1]); 
        //get job 
        job = getjobpid(jobs, pid);
        
        if (job == NULL) {
            printf("%s: No such job\n", argv[1]);
            return;
        }

    }

    // check of command is bg or fg 
    if (strcmp(argv[0], "bg") == 0) {
        job->state = BG;
        if (kill(-(job->pid), SIGCONT) == -1) {
            perror("kill");
            exit(-1);
        }  
        printf("[%d] (%d) %s", job->jid, job->pid, job->cmdline);

    }
    else if (strcmp(argv[0], "fg") == 0) {
        job->state = FG;
        pid_t current_pid = fgpid(jobs);

        if (current_pid != 0) {
            if (kill(-(job->pid), SIGCONT) == -1) {
                perror("kill");
                exit(-1);
            }
            waitfg(current_pid);
        }  

    }

    return;
    
}


// Aashka
/* 
 * waitfg - Block until process pid is no longer the foreground process
 */
void waitfg(pid_t pid) {

    sigset_t mask, prev_mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);

    // Block SIGCHLD and save current signal mask to avoid race conditions 
    sigprocmask(SIG_BLOCK, &mask, &prev_mask);

    // Wait until the foreground process is no longer in the foreground
    while (pid == fgpid(jobs)) {
        // Wait for a signal
        sigsuspend(&prev_mask);
    }

    // Restore the previous signal mask
    sigprocmask(SIG_SETMASK, &prev_mask, NULL);
} 




/*****************
 * Signal handlers
 *****************/

// Alan
/* 
 * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 *     a child job terminates (becomes a zombie), or stops because it
 *     received a SIGSTOP or SIGTSTP signal. The handler reaps all
 *     available zombie children, but doesn't wait for any other
 *     currently running children to terminate.  
 */
void sigchld_handler(int sig) {
    // listjobs(jobs);
    pid_t j;
    while ((j = waitpid(-1, NULL, WNOHANG)) > 0) {
        // printf("LOOP");
        deletejob(jobs, j);
        continue;
    }
    return;
}

// Alan
/* 
 * sigint_handler - The kernel sends a SIGINT to the shell whenever the
 *    user types ctrl-c at the keyboard.  Catch it and send it along
 *    to the foreground job.  
 */



void sigint_handler(int sig) {
    pid_t fg;

    if ((fg = fgpid(jobs)) != 0) {
        // printf("SIGINT CAUGHT\n");
        if (kill(-fg, SIGINT) == -1) {
            perror("kill");
            exit(-1);
        }
	struct job_t *job = getjobpid(jobs, fg);
	printf("Job [%d] (%d) terminated by signal 2\n", job->jid, job->pid);
    }

    return;
}

// Alan
/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard. Catch it and suspend the
 *     foreground job by sending it a SIGTSTP.  
 */
void sigtstp_handler(int sig) {
    pid_t fg;
    
    if ((fg = fgpid(jobs)) != 0) {
        // printf("SIGTSTP CAUGHT\n");
        if (kill(fg, SIGTSTP) == -1) {
            perror("kill");
            exit(-1);
        }
        struct job_t *job = getjobpid(jobs, fg);
        job->state = ST;
    }

    return;
}

/*
 * sigusr1_handler - child is ready
 */
void sigusr1_handler(int sig) {
    ready = 1;
}


/*********************
 * End signal handlers
 *********************/

/***********************************************
 * Helper routines that manipulate the job list
 **********************************************/

/* clearjob - Clear the entries in a job struct */
void clearjob(struct job_t *job) {
    job->pid = 0;
    job->jid = 0;
    job->state = UNDEF;
    job->cmdline[0] = '\0';
}

/* initjobs - Initialize the job list */
void initjobs(struct job_t *jobs) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
        clearjob(&jobs[i]);
}

/* freejid - Returns smallest free job ID */
int freejid(struct job_t *jobs) {
    int i;
    int taken[MAXJOBS + 1] = {0};
    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].jid != 0) 
        taken[jobs[i].jid] = 1;
    for (i = 1; i <= MAXJOBS; i++)
        if (!taken[i])
            return i;
    return 0;
}

/* addjob - Add a job to the job list */
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline) {
    int i;
    
    if (pid < 1)
        return 0;
    int free = freejid(jobs);
    if (!free) {
        printf("Tried to create too many jobs\n");
        return 0;
    }
    for (i = 0; i < MAXJOBS; i++) {
        if (jobs[i].pid == 0) {
            jobs[i].pid = pid;
            jobs[i].state = state;
            jobs[i].jid = free;
            strcpy(jobs[i].cmdline, cmdline);
            if(verbose){
                printf("Added job [%d] %d %s\n", jobs[i].jid, jobs[i].pid, jobs[i].cmdline);
            }
            return 1;
        }
    }
    return 0; /*suppress compiler warning*/
}

/* deletejob - Delete a job whose PID=pid from the job list */
int deletejob(struct job_t *jobs, pid_t pid) {
    int i;

    if (pid < 1)
        return 0;

    for (i = 0; i < MAXJOBS; i++) {
        if (jobs[i].pid == pid) {
            clearjob(&jobs[i]);
            return 1;
        }
    }
    return 0;
}

/* fgpid - Return PID of current foreground job, 0 if no such job */
pid_t fgpid(struct job_t *jobs) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].state == FG)
            return jobs[i].pid;
    return 0;
}

/* getjobpid  - Find a job (by PID) on the job list */
struct job_t *getjobpid(struct job_t *jobs, pid_t pid) {
    int i;

    if (pid < 1)
        return NULL;
    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].pid == pid)
            return &jobs[i];
    return NULL;
}

/* getjobjid  - Find a job (by JID) on the job list */
struct job_t *getjobjid(struct job_t *jobs, int jid) 
{
    int i;

    if (jid < 1)
        return NULL;
    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].jid == jid)
            return &jobs[i];
    return NULL;
}

/* pid2jid - Map process ID to job ID */
int pid2jid(pid_t pid) {
    int i;

    if (pid < 1)
        return 0;
    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].pid == pid) {
            return jobs[i].jid;
    }
    return 0;
}

/* listjobs - Print the job list */
void listjobs(struct job_t *jobs) {
    int i;
    
    for (i = 0; i < MAXJOBS; i++) {
        if (jobs[i].pid != 0) {
            printf("[%d] (%d) ", jobs[i].jid, jobs[i].pid);
            switch (jobs[i].state) {
                case BG: 
                    printf("Running ");
                    break;
                case FG: 
                    printf("Foreground ");
                    break;
                case ST: 
                    printf("Stopped ");
                    break;
                default:
                    printf("listjobs: Internal error: job[%d].state=%d ", 
                       i, jobs[i].state);
            }
            printf("%s", jobs[i].cmdline);
        }
    }
}
/******************************
 * end job list helper routines
 ******************************/


/***********************
 * Other helper routines
 ***********************/

/*
 * usage - print a help message and terminate
 */
void usage(void) {
    printf("Usage: shell [-hvp]\n");
    printf("   -h   print this message\n");
    printf("   -v   print additional diagnostic information\n");
    printf("   -p   do not emit a command prompt\n");
    exit(1);
}

/*
 * unix_error - unix-style error routine
 */
void unix_error(char *msg) {
    fprintf(stdout, "%s: %s\n", msg, strerror(errno));
    exit(1);
}

/*
 * app_error - application-style error routine
 */
void app_error(char *msg) {
    fprintf(stdout, "%s\n", msg);
    exit(1);
}

/*
 * Signal - wrapper for the sigaction function
 */
handler_t *Signal(int signum, handler_t *handler) {
    struct sigaction action, old_action;

    action.sa_handler = handler;  
    sigemptyset(&action.sa_mask); /* block sigs of type being handled */
    action.sa_flags = SA_RESTART; /* restart syscalls if possible */

    if (sigaction(signum, &action, &old_action) < 0)
        unix_error("Signal error");
    return (old_action.sa_handler);
}

/*
 * sigquit_handler - The driver program can gracefully terminate the
 *    child shell by sending it a SIGQUIT signal.
 */
void sigquit_handler(int sig) {
    exit(1);
}


/* 
 * tsh - A tiny shell program with job control
 * 
 */
#include <stdio.h>
#include <stdlib.h>
