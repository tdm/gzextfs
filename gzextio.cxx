/* gzextio.cxx */

#include "gzextdef.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#include <pthread.h>

#include <unistd.h>
#include <zlib.h>
#include <com_err.h>
#include <ext2fs/ext2_err.h>
#include <ext2fs/ext2_io.h>

#include "gzextio.h"

#include <string>
#include <vector>
#include <map>

using namespace std;

typedef map<unsigned long,unsigned char*> blockcache;
typedef vector<unsigned long> blockhistory;

/*
 * Cache roughly 1gb of block data:
 *   64kb * 16k = 1gb
 */
#define GZ_CACHE_BLOCKSIZE (1024*1024)
#define GZ_CACHE_SIZE (1024)

struct gz_cache_entry {
    unsigned long       block;
    unsigned char       data[GZ_CACHE_BLOCKSIZE];
};

struct gzio_priv {
    uint64_t                offset;
    pthread_mutex_t         lock;
    gzFile                  file;
    blockcache              cache;
    size_t                  index;
    blockhistory            history;
};
#define GZIO_PRIV(p) ((struct gzio_priv*)(p->private_data))

static unsigned char*
fetch_block(struct gzio_priv* priv, unsigned long block)
{
    blockcache::iterator itr = priv->cache.find(block);
    if (itr != priv->cache.end()) {
        return itr->second;
    }

    if (gzseek(priv->file, block * GZ_CACHE_BLOCKSIZE, SEEK_SET) == -1) {
        return NULL;
    }
    unsigned char* data = new unsigned char[GZ_CACHE_BLOCKSIZE];
    if (gzread(priv->file, data, GZ_CACHE_BLOCKSIZE) != GZ_CACHE_BLOCKSIZE) {
        delete[] data;
        return NULL;
    }

    if (priv->cache.size() == GZ_CACHE_SIZE) {
        unsigned long evict = priv->history[priv->index];
        itr = priv->cache.find(evict);
        delete[] itr->second;
        priv->cache.erase(itr);
    }

    priv->cache[block] = data;
    priv->history[priv->index] = block;
    priv->index = (priv->index + 1) % GZ_CACHE_SIZE;

    return data;
}

static int
gzio_do_read(struct gzio_priv* priv, void* buf, uint64_t off, uint64_t len)
{
    pthread_mutex_lock(&priv->lock);

    while (len > 0) {
        unsigned long block = off / GZ_CACHE_BLOCKSIZE;
        unsigned char* data = fetch_block(priv, block);
        if (!data) {
            pthread_mutex_unlock(&priv->lock);
            return -1;
        }
        uint64_t copyoff = off - (block * GZ_CACHE_BLOCKSIZE);
        uint64_t copylen = GZ_CACHE_BLOCKSIZE - copyoff;
        if (copylen > len) {
            copylen = len;
        }
        memcpy(buf, data + copyoff, copylen);
        off += copylen;
        len -= copylen;
        buf = (unsigned char*)buf + copylen;
    }

    pthread_mutex_unlock(&priv->lock);

    return 0;
}

static errcode_t
gzio_open(const char* name, int flags, io_channel* channel)
{
    gzFile gzf = NULL;
    io_channel io = NULL;
    struct gzio_priv* priv = NULL;

    gzf = gzopen(name, "rb");
    if (!gzf) {
        return errno;
    }

    io = new struct_io_channel;
    memset(io, 0, sizeof(*io));
    io->magic = EXT2_ET_MAGIC_IO_CHANNEL;
    io->manager = const_cast<io_manager>(gzext_io_manager);
    io->name = strdup(name);
    io->block_size = 1024;
    io->refcount = 1;

    priv = new gzio_priv;
    memset(priv, 0, sizeof(*priv));
    pthread_mutex_init(&priv->lock, NULL);
    priv->file = gzf;
    priv->cache = blockcache();
    priv->history = blockhistory(GZ_CACHE_SIZE);

    io->private_data = priv;

    *channel = io;

    return 0;
}

static errcode_t
gzio_close(io_channel channel)
{
    struct gzio_priv* priv = GZIO_PRIV(channel);

    channel->private_data = NULL;
    gzclose(priv->file);
    pthread_mutex_destroy(&priv->lock);
    delete priv;

    return 0;
}

static errcode_t
gzio_set_blksize(io_channel channel, int blksize)
{
    channel->block_size = blksize;

    return 0;
}

static errcode_t
gzio_read_blk(io_channel channel, unsigned long block, int count, void* data)
{
    struct gzio_priv* priv = GZIO_PRIV(channel);
    uint64_t off;
    uint64_t len;

    off = priv->offset + block * channel->block_size;
    len = (count < 0) ? -count : count * channel->block_size;

    return gzio_do_read(priv, data, off, len);
}

static errcode_t
gzio_write_blk(io_channel channel, unsigned long block, int count, const void* data)
{
    return EXT2_ET_UNIMPLEMENTED;
}

static errcode_t
gzio_flush(io_channel channel)
{
    return 0;
}

static errcode_t
gzio_write_byte(io_channel channel, unsigned long offset, int count, const void* data)
{
    return EXT2_ET_UNIMPLEMENTED;
}

static errcode_t
gzio_set_option(io_channel channel, const char *option, const char *arg)
{
    struct gzio_priv* priv = GZIO_PRIV(channel);

    if (!strcmp(option, "offset")) {
        if (!arg) {
            return EXT2_ET_INVALID_ARGUMENT;
        }
        priv->offset = atoi(arg);
        return 0;
    }

    return EXT2_ET_INVALID_ARGUMENT;
}

static errcode_t
gzio_get_stats(io_channel channel, io_stats *io_stats)
{
    return EXT2_ET_UNIMPLEMENTED;
}

static errcode_t
gzio_read_blk64(io_channel channel, unsigned long long block, int count, void *data)
{
    struct gzio_priv* priv = GZIO_PRIV(channel);
    uint64_t off;
    uint64_t len;

    off = priv->offset + block * channel->block_size;
    len = (count < 0) ? -count : count * channel->block_size;

    return gzio_do_read(priv, data, off, len);
}

static errcode_t
gzio_write_blk64(io_channel channel, unsigned long long block, int count, const void *data)
{
    return EXT2_ET_UNIMPLEMENTED;
}

static errcode_t
gzio_discard(io_channel channel, unsigned long long block, unsigned long long count)
{
    return EXT2_ET_UNIMPLEMENTED;
}

#ifdef HAVE_EXT2FS_CACHE_READAHEAD
static errcode_t
gzio_cache_readahead(io_channel channel, unsigned long long block, unsigned long long count)
{
    return 0;
}
#endif

#ifdef HAVE_EXT2FS_ZEROOUT
static errcode_t
gzio_zeroout(io_channel channel, unsigned long long block, unsigned long long count)
{
    return EXT2_ET_UNIMPLEMENTED;
}
#endif

static const struct struct_io_manager struct_gzext_manager = {
    .magic              = EXT2_ET_MAGIC_IO_MANAGER,
    .name               = "gzip I/O Manager",
    .open               = gzio_open,
    .close              = gzio_close,
    .set_blksize        = gzio_set_blksize,
    .read_blk           = gzio_read_blk,
    .write_blk          = gzio_write_blk,
    .flush              = gzio_flush,
    .write_byte         = gzio_write_byte,
    .set_option         = gzio_set_option,
    .get_stats          = gzio_get_stats,
    .read_blk64         = gzio_read_blk64,
    .write_blk64        = gzio_write_blk64,
    .discard            = gzio_discard,
#ifdef HAVE_EXT2FS_CACHE_READAHEAD
    .cache_readahead    = gzio_cache_readahead,
#endif
#ifdef HAVE_EXT2FS_ZEROOUT
    .zeroout            = gzio_zeroout,
#endif
};

const struct_io_manager* gzext_io_manager = &struct_gzext_manager;
