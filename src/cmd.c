// SPDX-License-Identifier: BSD-3-Clause

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <unistd.h>

#include "cmd.h"
#include "utils.h"


#define READ		0
#define WRITE		1

/**
 * Internal change-directory command.
 */
static bool shell_cd(word_t *dir)
{
	/* Execute cd. */
	if ((dir == NULL) || (dir->next_part != NULL))
		return false;
	int rc = chdir(dir->string);

	if (rc < 0) {
		perror("cd");
		return false;
	}
	return true;
}

/**
 * Internal exit/quit command.
 */
static int shell_exit(void)
{
	/* Execute exit/quit. */
	return SHELL_EXIT;
}

/**
 * Open files for redirections.
 */
static void open_files(simple_command_t *s, int *outs_save)
{
	/* Define redirections, flags, and outputs. */
	int i, fd[3];
	word_t *redir[3] = {s->out, s->err, s->in};
	char *file;
	int flags[3] = {O_CREAT|O_WRONLY, O_CREAT|O_WRONLY, O_CREAT|O_RDONLY};
	int outs[3] = {STDOUT_FILENO, STDERR_FILENO, STDIN_FILENO};

	for (i = 0; i < 3; i++) {
		/* Save initial output. */
		outs_save[i] = dup(outs[i]);
		/* Check if we can redirect to out/err/in. */
		if (redir[i] != NULL) {
			file = get_word(redir[i]);
			/* Check for &>. If there's no &>, check flags for append/trunc. */
			if ((redir[1] != NULL) && (i == 0) && (!strcmp(get_word(redir[0]), get_word(redir[1])))) {
				fd[0] = open(file, flags[0]|O_APPEND|O_TRUNC, 0666);
				dup2(fd[0], outs[1]);
				redir[1] = NULL;
			} else if ((s->io_flags != 0) || (i == 2)) {
				fd[i] = open(file, flags[i]|O_APPEND, 0666);
			} else {
				fd[i] = open(file, flags[i]|O_TRUNC, 0666);
			}
			/* Redirect current output to files. */
			DIE(fd[i] < 0, "open");
			dup2(fd[i], outs[i]);
			close(fd[i]);
			free(file);
		}
	}
}

/**
 * Redirect output back to the terminal.
 */
static void fix_redirects(int *outs_save)
{
	int i;
	int outs[3] = {STDOUT_FILENO, STDERR_FILENO, STDIN_FILENO};

	for (i = 0; i < 3; i++) {
		dup2(outs_save[i], outs[i]);
		close(outs_save[i]);
	}
}

/**
 * Parse a simple command (internal, environment variable assignment,
 * external command).
 */
static int parse_simple(simple_command_t *s, int level, command_t *father)
{
	/* Sanity checks. */
	if (s->verb == NULL)
		return -1;
	pid_t child;
	int status = 0, size = 0, rc;
	int outs_save[3];
	char *command = get_word(s->verb), *var;

	/* If boolean value, return specific value.*/
	if (!strcmp(command, "true")) {
		free(command);
		return 0;
	} else if (!strcmp(command, "false")) {
		free(command);
		return -1;
	}

	/* If builtin command, execute the command. */
	if (!strcmp(command, "cd")) {
		open_files(s, outs_save);
		rc = !shell_cd(s->params);
		fix_redirects(outs_save);
		free(command);
		return rc;
	} else if (!strcmp(command, "exit") || !strcmp(command, "quit")) {
		free(command);
		return shell_exit();
	}
	/* If variable assignment, execute the assignment and return
	 * the exit status.
	 */
	if ((s->verb->next_part != NULL) && (!strcmp(s->verb->next_part->string, "="))) {
		var = get_word(s->verb->next_part->next_part);
		rc = setenv(s->verb->string, var, 1);
		free(command);
		free(var);
		return rc;
	}
	/* If external command:
	 *   1. Fork new process
	 *     2c. Perform redirections in child
	 *     3c. Load executable in child
	 *   2. Wait for child
	 *   3. Return exit status
	 */
	child = fork();
	DIE(child < 0, "fork");
	if (child == 0) {
		/* Open & Redirect. */
		open_files(s, outs_save);
		/* Execute. */
		rc = execvp(command, get_argv(s, &size));
		if (rc < 0) {
			fprintf(stderr, "Execution failed for '%s'\n", command);
			exit(EXIT_FAILURE);
		}
		exit(EXIT_SUCCESS);
	} else {
		wait(&status);
		free(command);
		return WEXITSTATUS(status);
	}
	free(command);
	return -1;
}

/**
 * Process two commands in parallel, by creating two children.
 */
static bool run_in_parallel(command_t *cmd1, command_t *cmd2, int level,
		command_t *father)
{
	/* Execute cmd1 and cmd2 simultaneously. */
	int rc, status = 0, i = 2;
	pid_t child1, child2;

	child1 = fork();
	/* Start first command. */
	DIE(child1 < 0, "fork");
	if (child1 == 0) {
		rc = parse_command(cmd1, level++, father);
		if (rc < 0)
			exit(EXIT_FAILURE);
		exit(EXIT_SUCCESS);
	} else {
		/* Start second command. */
		child2 = fork();
		DIE(child2 < 0, "fork");
		if (child2 == 0) {
			rc = parse_command(cmd2, level++, father);
			if (rc < 0)
				exit(EXIT_FAILURE);
			exit(EXIT_SUCCESS);
		} else {
			/* Parent process, waiting for both children.*/
			while (i != 0) {
				wait(&status);
				if (!WIFEXITED(status))
					return false;
				i--;
			}
		}
	}
	return true;
}

/**
 * Run commands by creating an anonymous pipe (cmd1 | cmd2).
 */
static bool run_on_pipe(command_t *cmd1, command_t *cmd2, int level,
		command_t *father)
{
	/* Redirect the output of cmd1 to the input of cmd2. */
	int fd[2], status = 0, rc;
	pid_t child1, child2;

	/* First, save redirects.*/
	rc = pipe(fd);
	DIE(rc < 0, "pipe");
	child1 = fork();
	DIE(child1 < 0, "fork");
	if (child1 == 0) {
		/* Get output from cmd1. */
		dup2(fd[1], STDOUT_FILENO);
		close(fd[0]);
		close(fd[1]);
		rc = parse_command(cmd1, level++, father);
		if (rc < 0)
			exit(EXIT_FAILURE);
		exit(EXIT_SUCCESS);
	} else {
		/* Execute cmd2 with output of cmd1 as input. */
		child2 = fork();
		DIE(child2 < 0, "fork");
		if (child2 == 0) {
			dup2(fd[0], STDIN_FILENO);
			close(fd[0]);
			close(fd[1]);
			rc = parse_command(cmd2, level++, father);
			if (rc < 0)
				exit(EXIT_FAILURE);
			exit(EXIT_SUCCESS);
		} else {
			/* Parent process, waiting for both children.*/
			close(fd[0]);
			close(fd[1]);
			waitpid(child1, &status, WSTOPPED);
			waitpid(child2, &status, WSTOPPED);
			if (WEXITSTATUS(status) != 0)
				return false;
		}
	}
	return true;
}

/**
 * Parse and execute a command.
 */
int parse_command(command_t *c, int level, command_t *father)
{
	/* sanity checks */
	if ((c->cmd1 == NULL) && (c->cmd2 == NULL) && (c->scmd == NULL))
		return -1;

	if (c->op == OP_NONE) {
		/* Replace with actual exit code of command. */
		return parse_simple(c->scmd, level++, father);
	}

	switch (c->op) {
	case OP_SEQUENTIAL:
		/* Execute the commands one after the other. */
		parse_command(c->cmd1, level++, c);
		parse_command(c->cmd2, level++, c);
		break;

	case OP_PARALLEL:
		/* Execute the commands simultaneously. */
		return !run_in_parallel(c->cmd1, c->cmd2, level++, c);

	case OP_CONDITIONAL_NZERO:
		/* Execute the second command only if the first one
		 * returns non zero.
		 */
		if (parse_command(c->cmd1, level++, c) != 0)
			parse_command(c->cmd2, level++, c);
		break;

	case OP_CONDITIONAL_ZERO:
		/* Execute the second command only if the first one
		 * returns zero.
		 */
		if (parse_command(c->cmd1, level++, c) == 0)
			parse_command(c->cmd2, level++, c);
		break;

	case OP_PIPE:
		/* Redirect the output of the first command to the
		 * input of the second.
		 */
		return !run_on_pipe(c->cmd1, c->cmd2, level++, c);

	default:
		return SHELL_EXIT;
	}

	/* Replace with actual exit code of command. */
	return -1;
}
