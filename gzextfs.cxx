/* gsextfs.cxx */

#include "gzextdef.h"

#include <stdlib.h>
#include <stdio.h>
#include <limits.h>

#include <unistd.h>
#include <sys/types.h>

#include <fuse/fuse.h>

extern "C" {
#include <ext2fs/ext2_err.h>
#include <ext2fs/ext2fs.h>
}
#ifndef HAVE_EXT2FS_DIRENT_NAME_LEN
static inline int
ext2fs_dirent_name_len(const struct ext2_dir_entry *entry)
{
    return entry->name_len & 0xff;
}
#endif

#include "gzextio.h"

#include <string>
#include <list>
#include <map>

using namespace std;

typedef list<string> stringlist;

typedef map<string, ext2_ino_t> pathcache;
typedef map<ext2_ino_t, ext2_inode> inodecache;
typedef map<ext2_ino_t, stringlist> dircache;

struct file_state {
    pthread_mutex_t lock;
};
typedef map<ext2_file_t, file_state*> filemap;

static struct options {
    const char* filename;
    int show_help;
} options;

struct gzfs_priv {
    char*           filename;
    ext2_filsys     fs;

    pthread_mutex_t path_lock;
    pathcache       path_cache;
    pthread_mutex_t inode_lock;
    inodecache      inode_cache;
    pthread_mutex_t dir_lock;
    dircache        dir_cache;

    pthread_mutex_t file_lock;
    filemap         file_table;
};
#define GZFS_PRIV(p) ((struct gzfs_priv*)(p->private_data))

/*** ext2 helpers ***/

static bool
lookup_path(struct gzfs_priv* priv, const char* pathname, ext2_ino_t& inum)
{
    const char* p;
    const char* q;
    ext2_ino_t dir;
    ext2_ino_t subdir;

    if (!*pathname) {
        inum = EXT2_ROOT_INO;
        return true;
    }

    pthread_mutex_lock(&priv->path_lock);

    pathcache::iterator itr = priv->path_cache.find(pathname);
    if (itr != priv->path_cache.end()) {
        inum = itr->second;
        pthread_mutex_unlock(&priv->path_lock);
        return true;
    }

    dir = EXT2_ROOT_INO;

    p = pathname;
    q = strchr(p, '/');
    while (q) {
        if (ext2fs_lookup(priv->fs, dir, p, q - p, NULL, &subdir) != 0) {
            pthread_mutex_unlock(&priv->path_lock);
            return false;
        }
        dir = subdir;
        p = q + 1;
        q = strchr(p, '/');
    }

    if (ext2fs_lookup(priv->fs, dir, p, strlen(p), NULL, &inum) != 0) {
        pthread_mutex_unlock(&priv->path_lock);
        return false;
    }

    priv->path_cache[pathname] = inum;

    pthread_mutex_unlock(&priv->path_lock);

    return true;
}

static bool
lookup_inode(struct gzfs_priv* priv, ext2_ino_t inum, ext2_inode& inode)
{
    pthread_mutex_lock(&priv->inode_lock);

    inodecache::iterator itr = priv->inode_cache.find(inum);
    if (itr != priv->inode_cache.end()) {
        inode = itr->second;
        pthread_mutex_unlock(&priv->inode_lock);
        return true;
    }

    if (ext2fs_read_inode(priv->fs, inum, &inode) != 0) {
        pthread_mutex_unlock(&priv->inode_lock);
        return false;
    }

    priv->inode_cache[inum] = inode;

    pthread_mutex_unlock(&priv->inode_lock);

    return true;
}

static int
dir_iterator(struct ext2_dir_entry* dirent, int offset,
             int blocksize, char* buf, void* priv)
{
    stringlist* filenames = (stringlist*)priv;

    filenames->push_back(string(dirent->name, ext2fs_dirent_name_len(dirent)));

    return 0;
}

static bool
lookup_dir(struct gzfs_priv* priv, ext2_ino_t inum, stringlist& filenames)
{
    pthread_mutex_lock(&priv->dir_lock);

    dircache::iterator itr = priv->dir_cache.find(inum);
    if (itr != priv->dir_cache.end()) {
        filenames = itr->second;
        pthread_mutex_unlock(&priv->dir_lock);
        return true;
    }

    if (ext2fs_dir_iterate(priv->fs, inum, 0, NULL, dir_iterator, &filenames) != 0) {
        pthread_mutex_unlock(&priv->dir_lock);
        return false;
    }

    priv->dir_cache[inum] = filenames;

    pthread_mutex_unlock(&priv->dir_lock);

    return true;
}

/*** FUSE operations ***/

static int
gzfs_getattr(const char* path, struct stat* st)
{
    struct gzfs_priv* priv = GZFS_PRIV(fuse_get_context());
    ext2_ino_t inum;
    struct ext2_inode inode;

    if (*path != '/') {
        return -1;
    }

    if (!lookup_path(priv, path + 1, inum)) {
        return -1;
    }

    if (!lookup_inode(priv, inum, inode)) {
        return -1;
    }

    st->st_ino = inum;
    st->st_mode = inode.i_mode;
    st->st_nlink = inode.i_links_count;
    st->st_uid = inode.i_uid;
    st->st_gid = inode.i_gid;
    st->st_size = inode.i_size;
    st->st_blksize = 4096 /* XXX */;
    st->st_blocks = inode.i_blocks;
    st->st_atime = inode.i_atime;
    st->st_mtime = inode.i_mtime;
    st->st_ctime = inode.i_ctime;

    return 0;
}

static int
gzfs_readlink(const char* path, char* buf, size_t size)
{
    struct gzfs_priv* priv = GZFS_PRIV(fuse_get_context());
    ext2_ino_t inum;
    struct ext2_inode inode;

    if (*path != '/') {
        return -1;
    }

    if (!lookup_path(priv, path+1, inum)) {
        return -1;
    }

    if (!lookup_inode(priv, inum, inode)) {
        return -1;
    }

    /* XXX: The below needs tested thoroughly. */
    char linkbuf[PATH_MAX + 1];
    size_t linksize;
    if (ext2fs_inode_has_valid_blocks(&inode)) {
        ext2_file_t file;
        unsigned int nread;
        if (ext2fs_file_open(priv->fs, inum, 0, &file) != 0) {
            return -1;
        }
        if (ext2fs_file_read(file, linkbuf, sizeof(linkbuf), &nread) != 0) {
            ext2fs_file_close(file);
            return -1;
        }
        linksize = nread;
        ext2fs_file_close(file);
    }
    else {
        memcpy(linkbuf, (const char*)inode.i_block, sizeof(inode.i_block));
        linksize = sizeof(inode.i_block);
    }

    if (size > linksize) {
        size = linksize;
    }
    memcpy(buf, linkbuf, size);

    return 0;
}

static int
gzfs_open(const char* path, struct fuse_file_info* ffi)
{
    struct gzfs_priv* priv = GZFS_PRIV(fuse_get_context());
    ext2_ino_t inum;
    ext2_file_t file;

    if (*path != '/') {
        return -1;
    }

    if (!lookup_path(priv, path + 1, inum)) {
        return -1;
    }

    if (ext2fs_file_open(priv->fs, inum, 0, &file) != 0) {
        return -1;
    }

    file_state* state = new file_state;
    pthread_mutex_init(&state->lock, NULL);

    pthread_mutex_lock(&priv->file_lock);
    priv->file_table[file] = state;
    pthread_mutex_unlock(&priv->file_lock);

    ffi->fh = (uint64_t)file;

    return 0;
}

static int
gzfs_read(const char* path, char* buf, size_t size, off_t off, struct fuse_file_info* ffi)
{
    struct gzfs_priv* priv = GZFS_PRIV(fuse_get_context());
    __u64 pos;
    unsigned int nread;
    ext2_file_t file = (ext2_file_t)ffi->fh;

    pthread_mutex_lock(&priv->file_lock);
    file_state* state = priv->file_table.find(file)->second;
    pthread_mutex_unlock(&priv->file_lock);

    pthread_mutex_lock(&state->lock);

    if (ext2fs_file_llseek(file, off, SEEK_SET, &pos) != 0) {
        pthread_mutex_unlock(&state->lock);
        return -1;
    }

    if (ext2fs_file_read(file, buf, size, &nread) != 0) {
        pthread_mutex_unlock(&state->lock);
        return -1;
    }

    pthread_mutex_unlock(&state->lock);

    return nread;
}

static int
gzfs_statfs(const char* param1, struct statvfs* param2)
{
    return -1;
}

static int
gzfs_flush(const char* param1, struct fuse_file_info* ffi)
{
    return 0;
}

static int
gzfs_release(const char* param1, struct fuse_file_info* ffi)
{
    struct gzfs_priv* priv = GZFS_PRIV(fuse_get_context());
    ext2_file_t file = (ext2_file_t)ffi->fh;

    pthread_mutex_lock(&priv->file_lock);
    filemap::iterator itr = priv->file_table.find(file);
    file_state* state = itr->second;
    priv->file_table.erase(itr);
    pthread_mutex_unlock(&priv->file_lock);

    pthread_mutex_destroy(&state->lock);
    delete state;

    ext2fs_file_close(file);

    return 0;
}

static int
gzfs_readdir(const char* path, void* buf, fuse_fill_dir_t filler, off_t off, struct fuse_file_info* ffi)
{
    struct gzfs_priv* priv = GZFS_PRIV(fuse_get_context());
    ext2_ino_t inum;

    if (*path != '/') {
        return -1;
    }

    if (!lookup_path(priv, path+1, inum)) {
        return -1;
    }

    stringlist filenames;
    if (!lookup_dir(priv, inum, filenames)) {
        return -1;
    }

    stringlist::iterator itr;
    for (itr = filenames.begin(); itr != filenames.end(); ++itr) {
        const string& name = *itr;
        if (filler(buf, name.c_str(), NULL, 0) != 0) {
            break;
        }
    }

    return 0;
}

static void*
gzfs_init(struct fuse_conn_info *conn)
{
    errcode_t rc;

    struct gzfs_priv* priv = new gzfs_priv;

    priv->filename = strdup(options.filename);
    pthread_mutex_init(&priv->path_lock, NULL);
    pthread_mutex_init(&priv->inode_lock, NULL);
    pthread_mutex_init(&priv->dir_lock, NULL);
    pthread_mutex_init(&priv->file_lock, NULL);

    rc = ext2fs_open(priv->filename,
                     0 /* flags */,
                     0 /* superblock */,
                     4096 /* block_size */,
                     const_cast<io_manager>(gzext_io_manager),
                     &priv->fs);
    if (rc != 0) {
        fprintf(stderr, "Failed to open ext2 filesystem on %s\n", priv->filename);
        exit(1);
    }

    return priv;
}

static void
gzfs_destroy(void* param)
{
    struct gzfs_priv* priv = GZFS_PRIV(fuse_get_context());

    ext2fs_close(priv->fs);

    delete priv;
}

static const struct fuse_operations gzfs_oper = {
    .getattr = gzfs_getattr,
    .readlink = gzfs_readlink,
    .getdir = NULL,
    .mknod = NULL,
    .mkdir = NULL,
    .unlink = NULL,
    .rmdir = NULL,
    .symlink = NULL,
    .rename = NULL,
    .link = NULL,
    .chmod = NULL,
    .chown = NULL,
    .truncate = NULL,
    .utime = NULL,
    .open = gzfs_open,
    .read = gzfs_read,
    .write = NULL,
    .statfs = gzfs_statfs,
    .flush = gzfs_flush,
    .release = gzfs_release,
    .fsync = NULL,
    .setxattr = NULL,
    .getxattr = NULL,
    .listxattr = NULL,
    .removexattr = NULL,
    .opendir = NULL,
    .readdir = gzfs_readdir,
    .releasedir = NULL,
    .fsyncdir = NULL,
    .init = gzfs_init,
    .destroy = gzfs_destroy,
    .access = NULL,
    .create = NULL,
    .ftruncate = NULL,
    .fgetattr = NULL,
    .lock = NULL,
    .utimens = NULL,
    .bmap = NULL,
    .flag_nullpath_ok = 0,
    .flag_nopath = 0,
    .flag_utime_omit_ok = 0,
    .flag_reserved = 0,
    .ioctl = NULL,
    .poll = NULL,
    .write_buf = NULL,
    .read_buf = NULL,
    .flock = NULL,
    .fallocate = NULL,
};

#define OPTION(t,p) \
    { t, offsetof(struct options, p), 1 }

static const struct fuse_opt option_spec[] = {
    OPTION("--filename=%s", filename),
    OPTION("-h", show_help),
    OPTION("--help", show_help),
    FUSE_OPT_END
};

static void
usage(const char* argv0)
{
    fprintf(stderr, "Usage: %s --filename=<image.gz> ... <mount>\n", argv0);
    exit(1);
}

int
main(int argc, char** argv)
{
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

    if ((getuid() == 0) || (geteuid() == 0)) {
        fprintf(stderr, "Refusing to run as root.\n");
        exit(1);
    }

    if (fuse_opt_parse(&args, &options, option_spec, NULL) == -1) {
        return 1;
    }

    if (options.show_help) {
        usage(argv[0]);
    }

    if (!options.filename) {
        usage(argv[0]);
    }
    char* real_filename = NULL;
    if (options.filename[0] == '/') {
        real_filename = strdup(options.filename);
    }
    else {
        real_filename = (char*)malloc(PATH_MAX + 1);
        if (realpath(options.filename, real_filename) == NULL) {
            fprintf(stderr, "Failed to resolve %s\n", options.filename);
            exit(1);
        }
        options.filename = real_filename;
    }

    return fuse_main(args.argc, args.argv, &gzfs_oper, NULL);
}
