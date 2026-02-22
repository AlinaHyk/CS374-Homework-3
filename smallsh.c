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

/* I was not sure what exactly I meant by “a lot of comments,” so I went to the extent of being as detailed as I could, just to be safe.
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