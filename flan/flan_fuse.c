// Copyright (C) 2021 Adam Manzanares <a.manzanares@samsung.com>

#define FUSE_USE_VERSION 39
#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <assert.h>
#include <libgen.h>

#include "flan.h"


static struct flan_fuse_info {
	const char *dev_uri;
	const char *mddev_uri;
	const char *poolname;
	uint64_t obj_sz;
	int usage;
} pf_info;

static const struct fuse_opt cmdln_opts[] = {
	{ "--dev_uri=%s", offsetof(struct flan_fuse_info, dev_uri), 1},
	{ "--mddev_uri=%s", offsetof(struct flan_fuse_info, mddev_uri), 1},
	{ "--poolname=%s", offsetof(struct flan_fuse_info, poolname), 1},
	{ "--obj_size=%lu", offsetof(struct flan_fuse_info, obj_sz), 1},
	{ "-h", offsetof(struct flan_fuse_info, usage), 1},
	{ "--help", offsetof(struct flan_fuse_info, usage), 1},
	FUSE_OPT_END
};

struct flan_handle *flanh;

static void *flan_fuse_init(struct fuse_conn_info *conn,
			struct fuse_config *cfg)
{
	int ret;
	(void) conn;
	cfg->kernel_cache = 1;

	ret = flan_init(pf_info.dev_uri, pf_info.mddev_uri, pf_info.poolname,
			  pf_info.obj_sz, &flanh);
	if (ret)
		printf("Something went wrong during init\n");

	return NULL;
}

static int flan_fuse_getattr(const char *path, struct stat *stbuf,
			       struct fuse_file_info *fi)
{
	int res = 0;
	struct flan_oinfo *oinfo;
	(void) fi;
	char *path_dup = strdup(path);
	uint32_t res_cur;

	memset(stbuf, 0, sizeof(struct stat));
	if (strcmp(path, "/") == 0) {
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;
		goto out;
	}

	oinfo = flan_find_oinfo(flanh, basename(path_dup), &res_cur);
	if (oinfo) {
		stbuf->st_mode = S_IFREG | 0444;
		stbuf->st_nlink = 1;
		stbuf->st_size = oinfo->size;
		stbuf->st_blocks = (oinfo->size / 512) + 1;
		goto out;
	}

	res = -ENOENT;
out:
	if (path_dup)
		free(path_dup);
	return res;
}

static int flan_fuse_readdir(const char *path, void *buf,
			       fuse_fill_dir_t filler, off_t offset,
			       struct fuse_file_info *fi,
			       enum fuse_readdir_flags flags)
{
	struct flan_oinfo *oinfo;
	(void) offset;
	(void) fi;
	(void) flags;

	if (strcmp(path, "/") != 0)
		return -ENOENT;

	flan_reset_pool_dir();
	while ((oinfo = flan_get_oinfo(flanh, false)) != NULL)
	{
		if (strlen(oinfo->name) > 0) {
			if (filler(buf, basename(oinfo->name), NULL, 0, 0))
				printf ("Filler had an issue\n");
		}
	}

	return 0;
}

static int flan_fuse_open(const char *path, struct fuse_file_info *fi)
{
	int ret = 0;

	if ((fi->flags & O_ACCMODE) != O_RDONLY)
		return -EACCES;

	ret = flan_object_open(path, flanh, &(fi->fh), 0);
	if (ret)
		return -EINVAL;

	return ret;
}

static int flan_fuse_read(const char *path, char *buf, size_t size,
			    off_t offset, struct fuse_file_info *fi)
{
	ssize_t len;

	if (offset + size > pf_info.obj_sz)
		size = pf_info.obj_sz - offset;

	len = flan_object_read(fi->fh, buf, offset, size, flanh);
	if (len < 0)
		return 0;

	return len;
}

static int flan_fuse_release(const char *path, struct fuse_file_info *fi)
{
	flan_object_close(fi->fh, flanh);
	return 0;
}

static const struct fuse_operations flan_oper = {
	.init		= flan_fuse_init,
	.getattr	= flan_fuse_getattr,
	.readdir	= flan_fuse_readdir,
	.open		= flan_fuse_open,
	.read		= flan_fuse_read,
	.release	= flan_fuse_release,
};

static void usage(const char *progname)
{
	printf("usage: %s [options] <mountpoint>\n\n", progname);
	printf("File-system specific options:\n"
	       "    --dev_uri=<s>	Name of the flan dev file\n"
	       "			(default: \"NULL\")\n"
	       "    --mddev_uri=<s>	Name of the flan md dev file\n"
	       "			(default: \"NULL\")\n"
	       "    --poolname=<s>	Name of the flan pool\n"
	       "			(default: \"NULL\")\n"
	       "    --obj_sz=<uint64>	Number of objects in the flan pool\n"
	       "			(default: \"NULL\")\n"
	       "\n");
}

int main(int argc, char *argv[])
{
	int ret;
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

	if (fuse_opt_parse(&args, &pf_info, cmdln_opts, NULL) == -1)
		return 1;

	if (pf_info.usage) {
		usage(argv[0]);
		assert(fuse_opt_add_arg(&args, "--help") == 0);
		args.argv[0][0] = '\0';
	}

	ret = fuse_main(args.argc, args.argv, &flan_oper, NULL);
	fuse_opt_free_args(&args);
	return ret;
}
