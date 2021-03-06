/*
 * rwc [-0cdep] [PATH...] - report when changed
 *  -0  use NUL instead of newline for input/output separator
 *  -c  detect all creations (open/O_CREAT, mkdir, link, symlink, bind)
 *  -d  detect deletions too (prefixed with "- ")
 *  -e  exit after the first reported change
 *  -p  pipe mode, don't generate new events if stdout pipe is not empty
 *
 * To the extent possible under law, Leah Neukirchen <leah@vuxu.org>
 * has waived all copyright and related or neighboring rights to this work.
 * http://creativecommons.org/publicdomain/zero/1.0/
 */

#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <search.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

char *argv0;
char ibuf[8192];
int ifd;

int cflag;
int dflag;
int eflag;
int pflag;
char input_delim = '\n';

static void *root = 0; // tree

static int
order(const void *a, const void *b)
{
	return strcmp((char *)a, (char *)b);
}

struct wdmap {
	int wd;
	char *dir;
};

static void *wds = 0; // tree

static int
wdorder(const void *a, const void *b)
{
	struct wdmap *ia = (struct wdmap *)a;
	struct wdmap *ib = (struct wdmap *)b;

	if (ia->wd == ib->wd)
		return 0;
	else if (ia->wd < ib->wd)
		return -1;
	else
		return 1;
}

static char *
realpath_nx(char *path)
{
	char *r = realpath(path, 0);

	if (!r && errno == ENOENT) {
		// resolve dirname and append basename

		char *path2 = strdup(path);
		if (!path2)
			return 0;
		char *d = realpath(dirname(path2), 0);
		free(path2);
		if (!d)
			return 0;
		char *b = basename(path);
		size_t l = strlen(d) + 1 + strlen(b) + 1;
		r = malloc(l);
		if (!r)
			return 0;
		snprintf(r, l, "%s/%s", d, b);
	}

	return r;
}

static void
add(char *path)
{
	struct stat st;
	int wd;

	char *file = realpath_nx(path);
	if (!file) {
		fprintf(stderr, "%s: realpath: %s: %s\n",
		    argv0, path, strerror(errno));
		return;
	}

	char *dir = file;

	tsearch(strdup(file), &root, order);

	// assume non-existing files are regular files
	if (lstat(file, &st) < 0 || !S_ISDIR(st.st_mode))
		dir = dirname(file);

	wd = inotify_add_watch(ifd, dir,
	    IN_MOVED_TO | IN_CLOSE_WRITE | cflag | dflag);
	if (wd < 0) {
		fprintf(stderr, "%s: inotify_add_watch: %s: %s\n",
		    argv0, dir, strerror(errno));
	} else {
		struct wdmap *newkey = malloc(sizeof (struct wdmap));
		newkey->wd = wd;
		newkey->dir = dir;
		tsearch(newkey, &wds, wdorder);
	}
}

int
main(int argc, char *argv[])
{
	int c, i;
	char *line = 0;

	argv0 = argv[0];

	while ((c = getopt(argc, argv, "0cdep")) != -1)
		switch (c) {
		case '0': input_delim = 0; break;
		case 'c': cflag = IN_CREATE; break;
		case 'd': dflag = IN_DELETE|IN_DELETE_SELF|IN_MOVED_FROM; break;
		case 'e': eflag++; break;
		case 'p': pflag++; break;
		default:
			fprintf(stderr, "Usage: %s [-0cdep] [PATH...]\n", argv0);
			exit(2);
		}

	ifd = inotify_init();
	if (ifd < 0) {
		fprintf(stderr, "%s: inotify_init: %s\n",
		    argv0, strerror(errno));
		exit(111);
	}

	i = optind;
	if (optind == argc)
		goto from_stdin;
	for (; i < argc; i++) {
		if (strcmp(argv[i], "-") != 0) {
			add(argv[i]);
			continue;
		}
from_stdin:
		while (1) {
			size_t linelen = 0;
			ssize_t rd;

			errno = 0;
			rd = getdelim(&line, &linelen, input_delim, stdin);
			if (rd == -1) {
				if (errno != 0)
					return -1;
				break;
			}

			if (rd > 0 && line[rd-1] == input_delim)
				line[rd-1] = 0;  // strip delimiter

			add(line);
		}
	}
	free(line);

	while (1) {
		ssize_t len, i;
		struct inotify_event *ev;

		len = read(ifd, ibuf, sizeof ibuf);
		if (len <= 0) {
			fprintf(stderr, "%s: error reading inotify buffer: %s",
			    argv0, strerror(errno));
			exit(1);
		}

		for (i = 0; i < len; i += sizeof (*ev) + ev->len) {
			ev = (struct inotify_event *)(ibuf + i);

			if (ev->mask & IN_IGNORED)
				continue;

			struct wdmap key, **result;
			key.wd = ev->wd;
			key.dir = 0;
			result = tfind(&key, &wds, wdorder);
			if (!result)
				continue;

			char *dir = (*result)->dir;
			char fullpath[PATH_MAX];
			char *name = ev->name;
			if (strcmp(dir, ".") != 0) {
				snprintf(fullpath, sizeof fullpath, "%s/%s",
				    dir, ev->name);
				name = fullpath;
			}

			if (tfind(name, &root, order) ||
			    tfind(dir, &root, order)) {
				if (pflag) {
					int n;
					ioctl(1, FIONREAD, &n);
					if (n > 0)
						break;
				}
				const char *mark = "";
				if (ev->mask & (IN_DELETE | IN_MOVED_FROM))
					mark = "- ";
				else if (ev->mask & IN_CREATE)
					mark = "+ ";
				printf("%s%s%c", mark, name, input_delim);
				fflush(stdout);
				if (eflag)
					exit(0);
			}
		}
	}

	return 0;
}
