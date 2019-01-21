#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <uthash.h>
#include <zlib.h>

#ifdef __APPLE__
#include "dyld-interposing.h"
#define ORIG(x) x
#define REPLACEMENT(x) replacement_ ## x
#else
#define ORIG(x) orig_ ## x
#define REPLACEMENT(x) x
#endif

#define die(fmt, ...) do { \
  fprintf (stderr, "zlib-dumper: " fmt "\n", ##__VA_ARGS__); \
  abort (); \
} while (0)

static int creat_or_die (const char *path)
{
  int fd;

  fd = creat (path, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
  if (fd == -1)
    die ("creat() failed");
  return fd;
}

static void write_or_die (int fd, const void *buf, size_t count)
{
  ssize_t ret;

  while (count)
    {
      ret = write (fd, buf, count);
      if (ret <= 0)
        die ("write() failed");
      count -= ret;
    }
}

static void close_or_die (int fd)
{
  if (close (fd) < 0)
    die ("close() failed");
}

struct hash_entry {
  z_streamp strm;
  int ifd;
  int ofd;
  UT_hash_handle hh;
};

static struct hash_entry *streams;
static atomic_ulong streams_counter;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

static struct hash_entry *add_stream_or_die (z_streamp strm, const char *kind)
{
  unsigned long counter;
  unsigned long pid;
  char path[256];
  struct hash_entry *p;

  p = calloc (1, sizeof (*p));
  if (!p)
    die ("oom");
  p->strm = strm;
  pid = (unsigned long) getpid ();
  counter = atomic_fetch_add(&streams_counter, 1);
  snprintf(path, sizeof (path), "%s.%lu.%lu.in", kind, pid, counter);
  p->ifd = creat_or_die (path);
  snprintf(path, sizeof (path), "%s.%lu.%lu.out", kind, pid, counter);
  p->ofd = creat_or_die (path);
  pthread_mutex_lock (&mutex);
  HASH_ADD (hh, streams, strm, sizeof (z_streamp), p);
  pthread_mutex_unlock (&mutex);
  return p;
}

static struct hash_entry *find_stream_or_die (z_streamp strm)
{
  struct hash_entry *p;

  pthread_mutex_lock (&mutex);
  HASH_FIND (hh, streams, &strm, sizeof (z_streamp), p);
  pthread_mutex_unlock (&mutex);
  if (!p)
    die ("unknown stream: %p", strm);
  return p;
}

static void end_stream_or_die (z_streamp strm)
{
  struct hash_entry *p;

  pthread_mutex_lock (&mutex);
  HASH_FIND (hh, streams, &strm, sizeof (z_streamp), p);
  if (p)
    HASH_DELETE (hh, streams, p);
  pthread_mutex_unlock (&mutex);
  if (!p)
    die ("unknown stream: %p", strm);
  close_or_die (p->ifd);
  close_or_die (p->ofd);
  free (p);
}

#ifndef __APPLE__
typedef int (*deflateInit_t) (z_streamp strm, int level,
                              const char *version, int stream_size);
typedef int (*deflateInit2_t) (z_streamp strm, int level, int method,
                               int windowBits, int memLevel,
                               int strategy, const char *version,
                               int stream_size);
typedef int (*deflate_t) (z_streamp strm, int flush);
typedef int (*deflateEnd_t) (z_streamp strm);
static deflateInit_t ORIG (deflateInit_);
static deflateInit2_t ORIG (deflateInit2_);
static deflate_t ORIG (deflate);
static deflateEnd_t ORIG (deflateEnd);

static void *dlsym_or_die (const char *name)
{
  void *sym;

  sym = dlsym (RTLD_NEXT, name);
  if (!sym)
    die ("could not resolve \"%s\"", name);
  return sym;
}

__attribute__((constructor))
static void init ()
{
  ORIG (deflateInit_) = (deflateInit_t) dlsym_or_die ("deflateInit_");
  ORIG (deflateInit2_) = (deflateInit2_t) dlsym_or_die ("deflateInit2_");
  ORIG (deflate) = (deflate_t) dlsym_or_die ("deflate");
  ORIG (deflateEnd) = (deflateEnd_t) dlsym_or_die ("deflateEnd");
}
#endif

extern int REPLACEMENT (deflateInit_) (z_streamp strm, int level,
                                       const char *version, int stream_size)
{
  int err;

  err = ORIG(deflateInit_) (strm, level, version, stream_size);
  if (err == Z_OK)
    add_stream_or_die (strm, "deflate");
  return err;
}

extern int REPLACEMENT (deflateInit2_) (z_streamp strm, int level, int method,
                                        int windowBits, int memLevel,
                                        int strategy, const char *version,
                                        int stream_size)
{
  int err;

  err = ORIG(deflateInit2_) (strm, level, method,
                             windowBits, memLevel,
                             strategy, version,
                             stream_size);
  if (err == Z_OK)
    add_stream_or_die (strm, "deflate");
  return err;
}

extern int REPLACEMENT (deflate) (z_streamp strm, int flush)
{
  struct hash_entry *stream;
  z_const Bytef *next_in;
  Bytef *next_out;
  int err;

  stream = find_stream_or_die (strm);
  next_in = strm->next_in;
  next_out = strm->next_out;
  err = ORIG(deflate) (strm, flush);
  write_or_die (stream->ifd, next_in, strm->next_in - next_in);
  write_or_die (stream->ofd, next_out, strm->next_out - next_out);
  return err;
}

extern int REPLACEMENT (deflateEnd) (z_streamp strm)
{
  end_stream_or_die (strm);
  return ORIG(deflateEnd) (strm);
}

#ifdef __APPLE__
DYLD_INTERPOSE (REPLACEMENT (deflateInit_), deflateInit_);
DYLD_INTERPOSE (REPLACEMENT (deflateInit2_), deflateInit2_);
DYLD_INTERPOSE (REPLACEMENT (deflate), deflate);
DYLD_INTERPOSE (REPLACEMENT (deflateEnd), deflateEnd);
#endif
