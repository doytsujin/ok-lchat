/*
 * Copyright (c) 2015-2016 Jan Klemkow <j.klemkow@wemelug.de>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/ioctl.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <term.h>
#include <termios.h>
#include <unistd.h>

#include "slackline.h"

#ifndef INFTIM
#define INFTIM -1
#endif

struct termios origin_term;
struct winsize winsize;

static void
sigwinch(int sig)
{
	if (sig == SIGWINCH)
		ioctl(STDOUT_FILENO, TIOCGWINSZ, &winsize);
}

static void
exit_handler(void)
{
	if (tcsetattr(STDIN_FILENO, TCSANOW, &origin_term) == -1)
		err(EXIT_FAILURE, "tcsetattr");
}

static char *
read_file_line(const char *file)
{
	FILE *fh;
	char buf[BUFSIZ];
	char *line = NULL;
	char *nl = NULL;

	if (access(file, R_OK) == -1)
		return NULL;

	if ((fh = fopen(file, "r")) == NULL)
		err(EXIT_FAILURE, "fopen");

	if (fgets(buf, sizeof buf, fh) == NULL)
		err(EXIT_FAILURE, "fgets");

	if (fclose(fh) == EOF)
		err(EXIT_FAILURE, "fclose");

	if ((nl = strchr(buf, '\n')) != NULL)	/* delete new line */
		*nl = '\0';

	if ((line = strdup(buf)) == NULL)
		err(EXIT_FAILURE ,"strdup");

	return line;
}

static bool
bell_match(const char *str, const char *regex_file)
{
	FILE *fh = NULL;
	char cmd[BUFSIZ];

	if (access(regex_file, R_OK) == -1)
		return true;

	snprintf(cmd, sizeof cmd, "exec grep -qf %s", regex_file);

	if ((fh = popen(cmd, "w")) == NULL)
		err(EXIT_FAILURE, "popen");

	if (fputs(str, fh) == EOF)
		err(EXIT_FAILURE, "fputs");

	if (pclose(fh) == 0)
		return true;

	return false;
}

static void
line_output(struct slackline *sl, char *file)
{
	int fd;

	if ((fd = open(file, O_WRONLY|O_APPEND)) == -1)
		err(EXIT_FAILURE, "open: %s", file);

	/* replace NUL-terminator with newline as line separator for file */
	sl->buf[sl->len] = '\n';

	if (write(fd, sl->buf, sl->len + 1) == -1)
		err(EXIT_FAILURE, "write");

	if (close(fd) == -1)
		err(EXIT_FAILURE, "close");
}

static void
usage(void)
{
	fprintf(stderr, "lchar [-aeh] [-n lines] [-p prompt] [-t title] [-i in]"
	    " [-o out] [directory]\n");
	exit(EXIT_FAILURE);
}

int
main(int argc, char *argv[])
{
	char tail_cmd[BUFSIZ];
	struct pollfd pfd[2];
	struct termios term;
	struct slackline *sl = sl_init();
	int fd = STDIN_FILENO;
	int c;
	int ch;
	bool empty_line = false;
	bool bell_flag = true;
	char *bell_file = ".bellmatch";
	size_t history_len = 5;
	char *prompt = read_file_line(".prompt");
	char *title = read_file_line(".title");

	if (prompt == NULL)	/* set default prompt */
		prompt = ">";

	size_t prompt_len = strlen(prompt);
	size_t loverhang = 0;
	char *dir = ".";
	char *in_file = NULL;
	char *out_file = NULL;
	FILE *tail_fh;

	while ((ch = getopt(argc, argv, "an:i:eo:p:t:h")) != -1) {
		switch (ch) {
		case 'a':
			bell_flag = false;
			break;
		case 'n':
			errno = 0;
			history_len = strtoull(optarg, NULL, 0);
			if (errno != 0)
				err(EXIT_FAILURE, "strtoull");
			break;
		case 'i':
			if ((in_file = strdup(optarg)) == NULL)
				err(EXIT_FAILURE, "strdup");
			break;
		case 'e':
			empty_line = true;
			break;
		case 'o':
			if ((out_file = strdup(optarg)) == NULL)
				err(EXIT_FAILURE, "strdup");
			break;
		case 'p':
			if ((prompt = strdup(optarg)) == NULL)
				err(EXIT_FAILURE, "strdup");
			prompt_len = strlen(prompt);
			break;
		case 't':
			if ((title = strdup(optarg)) == NULL)
				err(EXIT_FAILURE, "strdup");
			break;
		case 'h':
		default:
			usage();
			/* NOTREACHED */
		}
	}
	argc -= optind;
	argv += optind;

	if (argc > 1)
		usage();

	if (argc == 1)
		if ((dir = strdup(argv[0])) == NULL)
			err(EXIT_FAILURE, "strdup");

	if (in_file == NULL)
		if (asprintf(&in_file, "%s/in", dir) == -1)
			err(EXIT_FAILURE, "asprintf");

	if (out_file == NULL)
		if (asprintf(&out_file, "%s/out", dir) == -1)
			err(EXIT_FAILURE, "asprintf");

	if (isatty(fd) == 0)
		err(EXIT_FAILURE, "isatty");

	/* set optarg to terminal's window title */
	if (title != NULL) {
		if (strcmp(getenv("TERM"), "screen") == 0)
			printf("\033k%s\033\\", title);
		else
			printf("\033]0;%s\a", title);
	}

	/* preprate terminal reset on exit */
	if (tcgetattr(fd, &origin_term) == -1)
		err(EXIT_FAILURE, "tcgetattr");

	if (atexit(exit_handler) == -1)
		err(EXIT_FAILURE, "atexit");

	/* prepare terminal */
	if (tcgetattr(fd, &term) == -1)
		err(EXIT_FAILURE, "tcgetattr");

	/* TODO: clean up this block.  copied from cfmakeraw(3) */
	term.c_iflag &= ~(IMAXBEL|BRKINT|PARMRK|ISTRIP|INLCR|IGNCR|ICRNL|IXON);
//        term.c_oflag &= ~OPOST;
	term.c_lflag &= ~(ECHO|ECHONL|ICANON|IEXTEN);
	term.c_cflag &= ~(CSIZE|PARENB);
	term.c_cflag |= CS8;
	term.c_cc[VMIN] = 1;
	term.c_cc[VTIME] = 0;

	if (tcsetattr(fd, TCSANOW, &term) == -1)
		err(EXIT_FAILURE, "tcsetattr");

	/* get the terminal size */
	sigwinch(SIGWINCH);
	signal(SIGWINCH, sigwinch);

	setbuf(stdin, NULL);
	setbuf(stdout, NULL);

	/* open external source */
	snprintf(tail_cmd, sizeof tail_cmd, "exec tail -n %zd -f %s",
	    history_len, out_file);

	if (access(".filter", X_OK) == 0)
		strlcat(tail_cmd, " | ./.filter", sizeof tail_cmd);

	if ((tail_fh = popen(tail_cmd, "r")) == NULL)
		err(EXIT_FAILURE, "unable to open pipe to tail command");

	pfd[0].fd = fd;
	pfd[0].events = POLLIN;

	pfd[1].fd = fileno(tail_fh);
	pfd[1].events = POLLIN;

	/* print initial prompt */
	fputs(prompt, stdout);

	for (;;) {
		poll(pfd, 2, INFTIM);

		/* moves cursor back after linewrap */
		if (loverhang > 0) {
			fputs("\r\033[2K", stdout);
			printf("\033[%zuA", loverhang);
		}

		/* carriage return and erase the whole line */
		fputs("\r\033[2K", stdout);

		/* handle keyboard intput */
		if (pfd[0].revents & POLLIN) {
			c = getchar();
			if (c == 13) {	/* return */
				if (sl->len == 0 && empty_line == false)
					goto out;
				line_output(sl, in_file);
				sl_reset(sl);
			}
			if (sl_keystroke(sl, c) == -1)
				errx(EXIT_FAILURE, "sl_keystroke");
		}

		/* handle tail command error and its broken pipe */
		if (pfd[1].revents & POLLHUP)
			break;

		/* handle file intput */
		if (pfd[1].revents & POLLIN) {
			char buf[BUFSIZ];
			ssize_t n = read(pfd[1].fd, buf, sizeof buf);
			if (n == 0)
				errx(EXIT_FAILURE, "tail command exited");
			if (n == -1)
				err(EXIT_FAILURE, "read");
			if (write(STDOUT_FILENO, buf, n) == -1)
				err(EXIT_FAILURE, "write");

			/* terminate the input buffer with NUL */
			buf[n == BUFSIZ ? n - 1 : n] = '\0';

			/* ring the bell on external input */
			if (bell_flag && bell_match(buf, bell_file))
				putchar('\a');
		}
 out:
		/* show current input line */
		fputs(prompt, stdout);
		fputs(sl->buf, stdout);

		/* save amount of overhanging lines */
		loverhang = (prompt_len + sl->len) / winsize.ws_col;

		/* correct line wrap handling */
		if ((prompt_len + sl->len) > 0 &&
		    (prompt_len + sl->len) % winsize.ws_col == 0)
			fputs("\n", stdout);

		if (sl->cur < sl->len) {	/* move the cursor */
			putchar('\r');
			/* HACK: because \033[0C does the same as \033[1C */
			if (sl->cur + prompt_len > 0)
				printf("\033[%zuC", sl->cur + prompt_len);
		}
	}
	return EXIT_SUCCESS;
}
