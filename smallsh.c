/* Alina Hyk for HW3 in CS374 */


#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>

/* I was not sure what exactly I meant by a lot of comments and deantils expalions, so I went to the extent of being as detailed as I could, just to be safe.
I have a bit of dyslexia, so if there are any minor spelling mistakes, I apologize. */


/* max command length is 2048 chars per the spec, plus one for the null
   terminator; thw args cap is generous since spec only requires 20 but 512
   costs us basically nothing. bg procs cap is just to prevent unbounded
   array growth. */
#define MAX_CMD_LENGTH 2049
#define MAX_ARGS 512
#define MAX_BG_PROCS 256

/* this is the global that the SIGTSTP handler toggles to switch between normal mode and foreground-only mode. it has to be volatile sig_atomic_t and not
   just a regular int because signal handlers are asynchronous, meanign that they can fire at any point during 
   the execution, even in the middle of reading or writing a variable. 
   So the volatile premvents it form cosnotly tryign to optimize it.
   sig_atomic_t guarantees that reads and writes are atomic so we can't get a half-written value and the
   handler sets it to 0 or 1, and the main loop checks it before deciding
   whether to honor the & background flag.
   So in retuslt we don't modify it anywhere else in the program, just check it, which avoids reentrancy issues. */
volatile sig_atomic_t foreground_only_mode = 0;

/* just a flat array to keep track of background child PIDs (every time we fork
   a background process we add its PID here, and at the top of every loop
   iteration we scan through and waitpid with WNOHANG to see if any of them
   finished) */
pid_t bg_pids[MAX_BG_PROCS];
int bg_count = 0;

/* this is the signal handler for SIGTSTP, which gets sent when the user presses Ctrl-Z. 
All it does is flip the foreground_only_mode flag and print a message saying what happened.  */
void handle_SIGTSTP(int signo) {
    if (foreground_only_mode == 0) {
        char *message = "\nEntering foreground-only mode (& is now ignored)\n";
        write(STDOUT_FILENO, message, 50);
        foreground_only_mode = 1;
    } else {
        char *message = "\nExiting foreground-only mode\n";
        write(STDOUT_FILENO, message, 30);
        foreground_only_mode = 0;
    }
}

/* handles the $$ -> PID expansion that the spec requires. the approach is two passes over the string:
   first pass just counts how many "$$" pairs exist so  we know how much memory to allocate: what we do is each "$$" is 2 chars that gets replaced
   by the PID string which could be anywhere from 1 to like 7 chars, so the output string might be bigger or smaller than the input. 
   second pass then actually builds the output by walking through the input char by char (follwign structre above, when we see
   two $ in a row we copy the PID string instead, otherwise we just copy the char). 
   This allows us to correctly handle the left-to-right scanning the spec wants, 
   so for instce, "foo$$$" with PID 179 becomes "foo179$" because the first two $ pair up and  the third one is left alone. 
   Fucnion returns a malloc'd string that the caller needs to free when they're done with it. */
char *expand_dollar_dollar(const char *input, pid_t shell_pid) {
    char pid_str[32];
    snprintf(pid_str, sizeof(pid_str), "%d", shell_pid);
    int pid_len = strlen(pid_str);

    /* count how many $$ pairs we need to replace. strstr finds the next
       occurrence and we jump past it by 2 so $$$$ counts as two pairs. */
    int count = 0;
    const char *p = input;
    while ((p = strstr(p, "$$")) != NULL) {
        count++;
        p += 2;
    }

    int new_len = strlen(input) + count * (pid_len - 2) + 1;
    char *result = malloc(new_len);
    if (!result) {
        perror("malloc");
        exit(1);
    }

    /* second pass builds the actual expanded string */
    const char *src = input;
    char *dst = result;
    while (*src) {
        if (src[0] == '$' && src[1] == '$') {
            memcpy(dst, pid_str, pid_len);
            dst += pid_len;
            src += 2;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
    return result;
}

/* this will run at the top of every loop iteration to check whether any background
   processes have finished. I am using WNOHANG so that we can do the return immediately:
   spesifialy, it returns the child's PID if the child is done, or 0 if it's still running, or -1 on error. 
   This is imporant bacause we then can just quickly scan through all our tracked background PIDs without blocking.
   
   Also, when a child has terminated we need to figure out how it terminated and print the right message. 
   I'm using WIFEXITED which checks if it exited normally (called exit() or
   returned from main), and if so WEXITSTATUS gives us the exit code. 
   Spesially, agian, WIFSIGNALED checks if it was killed by a signal, and WTERMSIG gives the actual signal number.

   Fianlly, for the removal from the array i do a swap-and-shrink by replacing the terminated
   PID with the last element in the array and decrement the count (incrementing i after a
   swap because the element we just moved into position i hasn't been checked yet). */
void check_background_processes(void) {
    int i = 0;
    while (i < bg_count) {
        int child_status;
        pid_t result = waitpid(bg_pids[i], &child_status, WNOHANG);
        if (result > 0) {
            if (WIFEXITED(child_status)) {
                printf("background pid %d is done: exit value %d\n",
                       bg_pids[i], WEXITSTATUS(child_status));
            } else if (WIFSIGNALED(child_status)) {
                printf("background pid %d is done: terminated by signal %d\n",
                       bg_pids[i], WTERMSIG(child_status));
            }
            fflush(stdout);
            bg_pids[i] = bg_pids[bg_count - 1];
            bg_count--;
        } else {
            i++;
        }
    }
}


/* This will send SIGKILL to every background process we're tracking. 
   When the user runs exitk, the kill() sends a signal to a given PID, and SIGKILL (signal 9)
   is special because the target process can't catch or ignore it, since it's an
   unconditional termination. */
void kill_all_background(void) {
    for (int i = 0; i < bg_count; i++) {
        kill(bg_pids[i], SIGKILL);
    }
}

int main(void) {
    char cmd_line[MAX_CMD_LENGTH];
    char *args[MAX_ARGS];
    int last_fg_status = 0;
    int last_fg_signal = -1;

    /* getpid returns the PID of this process (after we got it the firt tiem we can reuse it
       for all $$ expansions since it never changes). */
    pid_t shell_pid = getpid();

    /* setting up signal handling for SIGINT (Ctrl-C).
       the parent shell ignores SIGINT entirely (sicne pressing Ctrl-C at the prompt
       should do nothing to smallsh). The children inherit this SIG_IGN by default when we fork, which is what we want for background children, i belive. 
       For foreground children however, we'll override it back to SIG_DFL so they die on Ctrl-C like normal. 
       Sigfillset on sa_mask means basily soemthing like "block every other signal for now since this handler is active"; Not sure if 
       it is required for the code, but I added to mke sure everythign is clean. */
    struct sigaction sa_sigint = {0};
    sa_sigint.sa_handler = SIG_IGN;
    sigfillset(&sa_sigint.sa_mask);
    sa_sigint.sa_flags = 0;
    sigaction(SIGINT, &sa_sigint, NULL);

    /* setting up SIGTSTP (Ctrl-Z) to use our custom handler that toggles foreground-only mode. 
       the SA_RESTART flag is used so that if the user presses Ctrl-Z while we will nto be sitting inside fgets
       waiting for input, as if it woudl have been otherwise, isntd, due to SA_RESTART flag fgets gets interrupted by the signal handler 
       and when the handler returns, fgets would return NULL with errno set to EINTR (instead agian of continuing to wait for input). 
       SA_RESTART also nciely tells the kernel to just restart the fgets call automatically after the handler finishes. 
       sigfillset allows to block other signals during the handler, which matters because the handler modifies the
       foreground_only_mode global and we don't want to get interrupted in the middle of that toggle, so that was the rational for that part. */
    struct sigaction sa_sigtstp = {0};
    sa_sigtstp.sa_handler = handle_SIGTSTP;
    sigfillset(&sa_sigtstp.sa_mask);
    sa_sigtstp.sa_flags = SA_RESTART;
    sigaction(SIGTSTP, &sa_sigtstp, NULL);

    /* main shell loop. runs until the user types exit or we hit EOF. */
    while (1) {
        /* check on background processes before prompting (this is where the
           "background pid X is done" messages show up) */
        check_background_processes();

        /* print the colon prompt. 
           fflush is necessary here becausa stdout is line-buffered by default, so without a newline character the colon
           would just sit in the buffer and the user wouldn't see it. */
        printf(":");
        fflush(stdout);

        /* thsi part reads user input. 
           I used memset as calening, sicne it zeros out the buffer first so we have a
           clean slate. fgets then reads up to MAX_CMD_LENGTH-1 chars and null- terminates; it returns NULL on either EOF 
           (Ctrl-D or end of redirected input) or on error (like EINTR from a signal, though SA_RESTART should handle most of those). 
           Then for EOF we break out of the loop, and for errors we clearerr so stdin works again and reprompts. */
        memset(cmd_line, '\0', MAX_CMD_LENGTH);
        if (fgets(cmd_line, MAX_CMD_LENGTH, stdin) == NULL) {
            if (feof(stdin)) {
                break;
            }
            clearerr(stdin);
            printf("\n");
            continue;
        }

        /* strips the trailing newline that fgets includes */
        size_t len = strlen(cmd_line);
        if (len > 0 && cmd_line[len - 1] == '\n') {
            cmd_line[len - 1] = '\0';
        }

        if (strlen(cmd_line) == 0) {
            continue;
        }
        if (cmd_line[0] == '#') {
            continue;
        }


        /* expand $$ to the shell's PID everywhere in the command string */
        char *expanded = expand_dollar_dollar(cmd_line, shell_pid);

        /* tokenize the command string. we need to pull out the command name and its arguments (goes into args[]), any input/output redirection
           files (< and > operators), and whether it should run in the background (& at the end). strtok_r is the reentrant version of
           strtok. For al fo that is uses the saveptr, sicne it allows us to give it to track its position instead of internal static state.
           Basisiclay, the spec guarantees ordering of command first, then args, then redirection, then &, so if there are soem wired cases (agian, I'm not sure it 
           that's soe thign that we had to account for) then we dont have to build in any addional handaling for them. */
        char *input_file = NULL;
        char *output_file = NULL;
        int background = 0;
        int arg_count = 0;

        char *saveptr;
        char *token = strtok_r(expanded, " ", &saveptr);
        while (token != NULL) {
            if (strcmp(token, "<") == 0) {
                /* the token after < is the input file path */
                token = strtok_r(NULL, " ", &saveptr);
                input_file = token;
            } else if (strcmp(token, ">") == 0) {
                /* the token after > is the output file path */
                token = strtok_r(NULL, " ", &saveptr);
                output_file = token;
            } else if (strcmp(token, "&") == 0) {
                /* & should only appear as the very last token. */
                char *next = strtok_r(NULL, " ", &saveptr);
                if (next == NULL) {
                    background = 1;
                } else {
                    args[arg_count++] = token;
                    token = next;
                    continue;
                }
            } else {
                args[arg_count++] = token;
            }
            token = strtok_r(NULL, " ", &saveptr);
        }
        /* execvp needs a NULL-terminated argv array */
        args[arg_count] = NULL;

        if (arg_count == 0) {
            free(expanded);
            continue;
        }

        /* if foreground-only mode is on (from SIGTSTP toggle), ignore & */
        if (foreground_only_mode) {
            background = 0;
        }

        /* kills all background children and terminate. */
        if (strcmp(args[0], "exit") == 0) {
            kill_all_background();
            free(expanded);
            exit(0);
        }

        /* built-in: cd. chdir() is the syscall that changes the current working
           directory for this process, so with an argument we cd to that path, without one we go to $HOME. 
           getenv("HOME") pulls the home directory ath from the environment variables. chdir only affects this process, so
           the child processes forked later will inherit whatever cwd we've set. */
        if (strcmp(args[0], "cd") == 0) {
            if (arg_count > 1) {
                if (chdir(args[1]) != 0) {
                    perror("cd");
                }
            } else {
                char *home = getenv("HOME");
                if (home != NULL) {
                    if (chdir(home) != 0) {
                        perror("cd");
                    }
                }
            }
            free(expanded);
            continue;
        }

        /* built-in: status prints the exit code or signal number of the last
           foreground command,so if nothing has run yet, last_fg_status is 0 by default
           and last_fg_signal is -1 (meaning no signal), so it prints "exit value 0" which is correct per the specficoans that we were givn int he assimgant. */
        if (strcmp(args[0], "status") == 0) {
            if (last_fg_signal >= 0) {
                printf("terminated by signal %d\n", last_fg_signal);
            } else {
                printf("exit value %d\n", last_fg_status);
            }
            fflush(stdout);
            free(expanded);
            continue;
        }

        /* for everything that isn't a built-in we fork a child process, sot aht the fork() creates an almost-exact copy of the current process. 
           Spesially, it returns 0 in the new child, the child's PID in the parent, or -1 if something went wrong (like the system is out of process
           slots). Them after this point we have two processes running the same code, and we can use the return value to figure out which one we are. */
        pid_t spawn_pid = fork();

        if (spawn_pid == -1) {
            perror("fork");
            last_fg_status = 1;
            last_fg_signal = -1;
            free(expanded);
            continue;
        }

        if (spawn_pid == 0) {
            /*Thsi is in the child process, so everything here only affects
               the child, not the parent shell. 
               As dicussed above, the child inherited copies of the parent's signal dispositions, file descriptors, environment, and etc*/

            /* for foreground children we need to restore the default SIGINT behavior so Ctrl-C actually kills them. 
               So the parent set SIGINT to SIG_IGN, and children inherit that, adn without this override foreground commands would be unkillable with Ctrl-C. 
               Also, the background children keep SIG_IGN because we don't want Ctrl-C killing background jobs. */
            if (!background) {
                struct sigaction sa_child_int = {0};
                sa_child_int.sa_handler = SIG_DFL;
                sigfillset(&sa_child_int.sa_mask);
                sa_child_int.sa_flags = 0;
                sigaction(SIGINT, &sa_child_int, NULL);
            }

            /* all children (btoh the foreground and background), ignore SIGTSTP. */
            struct sigaction sa_child_tstp = {0};
            sa_child_tstp.sa_handler = SIG_IGN;
            sigfillset(&sa_child_tstp.sa_mask);
            sa_child_tstp.sa_flags = 0;
            sigaction(SIGTSTP, &sa_child_tstp, NULL);

            /* This is the input redirection: if the user gave us a file with <, we open it read-only and then use dup2 to make stdin
               point to that file instead of the terminal. dup2(fd_in, STDIN_FILENO) closes the old stdin and makes fd 0 a copy of fd_in, so any
               subsequent reads from stdin actually read from our file. 
               We also close the original fd_in after because to avodi any redundantcy, sicne stdin itself is the file. i
               If cans of the background process when the user didn't specify an input file, we redirect stdin to /dev/null instead, sicne we need to 
               make sure tht the background processes is not trying to read from the terminal while the shell is also using it. 
               Fianlly, if open fails we print an error and exit(1) from the child so the parent can pick up the failure via waitpid. */
            if (input_file != NULL) {
                int fd_in = open(input_file, O_RDONLY);
                if (fd_in == -1) {
                    fprintf(stderr, "cannot open %s for input\n", input_file);
                    exit(1);
                }
                if (dup2(fd_in, STDIN_FILENO) == -1) {
                    perror("dup2 stdin");
                    exit(1);
                }
                close(fd_in);
            } else if (background) {
                int fd_devnull = open("/dev/null", O_RDONLY);
                if (fd_devnull == -1) {
                    perror("open /dev/null");
                    exit(1);
                }
                dup2(fd_devnull, STDIN_FILENO);
                close(fd_devnull);
            }

            /* Thsi is the output redirection, whcih uses the same idea as input but for stdout (fd 1).
               SO O_WRONLY means write-only, O_CREAT creates the file if it doesn't exist, O_TRUNC empties the file if it does exist. 
               
               *the 0644 is the permission bits (rw-r--r--) for newly created files.

               Agian, here the idea is that the background processes with no output file get their stdout sent
               to /dev/null so they don't dump output into the terminal while the user is typing commands.  */
            if (output_file != NULL) {
                int fd_out = open(output_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd_out == -1) {
                    fprintf(stderr, "cannot open %s for output\n", output_file);
                    exit(1);
                }
                if (dup2(fd_out, STDOUT_FILENO) == -1) {
                    perror("dup2 stdout");
                    exit(1);
                }
                close(fd_out);
            } else if (background) {
                int fd_devnull = open("/dev/null", O_WRONLY);
                if (fd_devnull == -1) {
                    perror("open /dev/null");
                    exit(1);
                }
                dup2(fd_devnull, STDOUT_FILENO);
                close(fd_devnull);
            }

            /* execvp replaces this entire child process with the specified program. 
               The 'v' means we pass arguments as an array (args), and the 'p' means it searches PATH for the command so the user
               can type "ls" instead of "/bin/ls". And if execvp succeeds, the code below it never runs because this process is now a completely
               different program. However, if it fails (command doesn't exist, bad
               permissions, whatever), it returns -1 and,s ot aht we cna fall through to the
               error message. */
            execvp(args[0], args);

            fprintf(stderr, "%s: no such file or directory\n", args[0]);
            exit(1);
        }

        /* back in the parent process after fork. */
        if (!background) {
            /* This is a foreground command: waitpid with flags=0 blocks until the child terminates. t
            So waitpid with flags=0 blocks until the child terminates (that's what makes it foreground), the shell just waits. 
            But then once it returns, we can check how the child died: WIFEXITED/WEXITSTATUS for normal exits and their exit code, WIFSIGNALED/WTERMSIG for signal kills and which signal. 
            We then store this for the status built-in and if a signal killed it, we also print the signal number right away. */
            int child_status;
            waitpid(spawn_pid, &child_status, 0);

            if (WIFEXITED(child_status)) {
                last_fg_status = WEXITSTATUS(child_status);
                last_fg_signal = -1;
            } else if (WIFSIGNALED(child_status)) {
                last_fg_signal = WTERMSIG(child_status);
                last_fg_status = 0;
                printf("terminated by signal %d\n", last_fg_signal);
                fflush(stdout);
            }
        } else {
            /* background command: printing the PID and stash it in our tracking array. 
               The the child runs on its own and we'll pick up its termination status in check_background_processes
               at the top of the next loop iteration via waitpid with WNOHANG. */
            printf("background pid is %d\n", spawn_pid);
            fflush(stdout);
            if (bg_count < MAX_BG_PROCS) {
                bg_pids[bg_count++] = spawn_pid;
            }
        }

        free(expanded);
    }

    /* Fianlign cleaning up of all the processes */
    kill_all_background();
    return 0;
}


