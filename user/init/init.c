#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#define SHELL_PATH "/system/bin/dash"
#define EXEC_FAILED 127

// Keeps a shell running, and collects any orphans along the way.
int main(void) {
    for (;;) {
        pid_t shell = fork();

        if (shell == 0) {
            char *const argv[] = {SHELL_PATH, NULL};
            char *const env[] = {"PATH=/system/bin", "HOME=/", "SHELL=" SHELL_PATH, "PS1=$PWD$ ", NULL};

            // Its own group and the terminal to go with it, which it waits for otherwise.
            setpgid(0, 0);
            tcsetpgrp(STDIN_FILENO, getpid());
            execve(SHELL_PATH, argv, env);
            printf("init: failed to run %s\n", SHELL_PATH);
            exit(EXEC_FAILED);
        }
        if (shell < 0) {
            printf("init: fork failed\n");
            return 1;
        }

        int status;
        pid_t pid;
        while ((pid = wait(&status)) != shell)
            ;

        if (status == EXEC_FAILED)
            return 1;
    }
}
