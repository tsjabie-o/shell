#include "parser/ast.h"
#include "shell.h"
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define STDIN 0
#define STDOUT 1
#define PIPE_RD 0
#define PIPE_WR 1


// Keeping track of child processes in order to signal SIGINT
pid_t child_table[2];
int child_count;

void track_child(int pid, int remove) {
    if (remove == 0) {
        child_count++;
        child_table[child_count-1] = pid;
    } else {
        child_table[child_count - 1] = -1;
        child_count--;
    }
}

static void handler(int signum) {
    for (int i = 0; i < child_count; i++)
    {
        kill(child_table[i], signum);
    }
    
}

void initialize(void)
{
    /* This code will be called once at startup */
    if (prompt)
    prompt = "vush$ ";

    // CTRL + C signal handling
    struct sigaction sa;
    sa.sa_handler = handler;
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGINT, &sa, NULL) == -1)
    {
        perror("Something went wrong");
    }
}

void cd_command(char **argv) {
    if (chdir(argv[1]) != 0) {
            perror("Could not change directory");
        }
    return;
}

void exit_command(char **argv, size_t argc) {
    if (argc > 1){
        exit(atoi(argv[1]));
    } else {
        exit(0);
    }
}

void run_command(node_t *node)
{
    /* Print parsed input for testing - comment this when running the tests! */
    // print_tree(node);

    if (node->type == NODE_COMMAND) {
        char *program = node->command.program;
        char **argv = node->command.argv;
        size_t argc = node->command.argc;

        if (strcmp(program, "exit") == 0) {
            exit_command(argv, argc);
        }

        if (strcmp(program, "cd") == 0) {
            cd_command(argv);
        } else {
            int status;
            int pid = fork();

            switch (pid)
            {
            case -1:
                // fork failed
                perror("Could not fork process");
                break;
            case 0:
                // We are child
                execvp(program, argv);
                // If we get here, execvp() has failed. Handle error and exit process
                perror("Encountered error while trying to execute your command");
                exit(errno);
                break;
            default:
                // We are parent
                track_child(pid, 0);
                waitpid(pid, &status, 0);
                track_child(pid, 1);
                // if (WIFEXITED(status)) {
                //     if (WEXITSTATUS(status) != 0) {
                //         fprintf(stderr, "exit with code %d\n", WEXITSTATUS(status));
                //     }
                // }
                break;
            }
        }
    }

    if (node->type == NODE_SEQUENCE) {
        // We simply run the commands in the right order (recursive)
        run_command(node->sequence.first);
        run_command(node->sequence.second);
    }

    if (node->type == NODE_PIPE) {
        node_t **parts = node->pipe.parts;
        node_t *p1 = parts[0];
        node_t *p2 = parts[1];

        int fd[2];
        int wr_pid, rd_pid;
        pipe(fd);
        
        wr_pid = fork();
        switch (wr_pid)
        {
            case -1:
                // fork failed
                perror("Could not fork process");
                break;
            case 0:
                // We are child
                close(fd[PIPE_RD]);
                close(STDOUT);
                dup(fd[PIPE_WR]);

                if (strcmp(p1->command.program, "exit") == 0) {
                    exit_command(p1->command.argv, p1->command.argc);
                }

                if (strcmp(p1->command.program, "cd") == 0) {
                    cd_command(p1->command.argv);
                } else {

                    execvp(p1->command.program, p1->command.argv);

                    // If we get here, execvp has failed
                    perror("Encountered error while trying to execute your command");
                    exit(errno);
                    break;
                }
        }

        rd_pid = fork();
        switch (rd_pid)
        {
            case -1:
                // fork failed
                perror("Could not fork process");
                break;
            case 0:
                // We are child
                close(fd[PIPE_WR]);
                close(STDIN);
                dup(fd[PIPE_RD]);

                if (strcmp(p2->command.program, "exit") == 0) {
                    exit_command(p2->command.argv, p2->command.argc);
                }

                if (strcmp(p2->command.program, "cd") == 0) {
                    cd_command(p2->command.argv);
                } else {
                    
                    execvp(p2->command.program, p2->command.argv);

                    // If we get here, execvp has failed
                    perror("Encountered error while trying to execute your command");
                    exit(errno);
                    break;
                }
        }

        close(fd[PIPE_RD]);
        close(fd[PIPE_WR]);

        // Wait for finish
        int status;
        track_child(wr_pid, 0);
        track_child(rd_pid, 0);
        waitpid(wr_pid, &status, 0);
        track_child(wr_pid, 1);
        
        
        waitpid(rd_pid, &status, 0);
        track_child(rd_pid, 1);
    }
    
    if (node->type == NODE_REDIRECT) {
        // Reassign file descriptors
        int temp_fd = dup(node->redirect.fd);
        int new_fd;

        switch (node->redirect.mode)
        {
        case 0:
            new_fd = node->redirect.fd2;
            dup2(new_fd, node->redirect.fd);
            run_command(node->redirect.child);
            break;
        
        case 1:
            new_fd = open(node->redirect.target, O_RDWR);
            dup2(new_fd, node->redirect.fd);
            close(new_fd);
            run_command(node->redirect.child);
            break;

        case 2:
            new_fd = open(node->redirect.target, O_CREAT | O_TRUNC | O_RDWR, S_IRUSR | S_IWUSR);
            dup2(new_fd, node->redirect.fd);
            close(new_fd);
            run_command(node->redirect.child);
            break;

        case 3:
            new_fd = open(node->redirect.target, O_APPEND | O_RDWR);
            dup2(new_fd, node->redirect.fd);
            close(new_fd);
            run_command(node->redirect.child);
            break;
        }


        // Reassign file descrptors to original setting
        dup2(temp_fd, node->redirect.fd);
        close(temp_fd);
    }
    
    if (node->type == NODE_SUBSHELL) {
        int pid = fork();
        if (pid == 0) {
            // Child
            run_command(node->subshell.child);
            exit(0);
        } else if (pid != -1) {
            // Parent
            int status;
            track_child(pid, 0);
            waitpid(pid, &status, 0);
            track_child(pid, 1);
        } else {
            perror("Could not create subshell");
        }
    }

    if (node->type == NODE_DETACH) {
        int pid = fork();
        if (pid == 0) {
            // Child
            run_command(node->detach.child);
            exit(0);
        } else if (pid != -1) {
            // Parent
            // Signal catching prevents zombie process
            signal(SIGCHLD,SIG_IGN);
        } else {
            perror("Could not create detached command");
        }
    }

    if (prompt)
        prompt = "vush$ ";
}