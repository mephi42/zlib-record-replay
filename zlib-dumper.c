#define _GNU_SOURCE
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
#include <dlfcn.h>
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
  int mfd;
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
  snprintf(path, sizeof (path), "%s.%lu.%lu.meta", kind, pid, counter);
  p->mfd = creat_or_die (path);
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
    die ("unknown stream: %p", (void *) strm);
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
    die ("unknown stream: %p", (void *) strm);
  close_or_die (p->ifd);
  close_or_die (p->ofd);
  free (p);
}

#ifndef __APPLE__
#define DEFINE_INTERPOSE(x) static typeof (&x) ORIG (x)
DEFINE_INTERPOSE (deflateInit_);
DEFINE_INTERPOSE (deflateInit2_);
DEFINE_INTERPOSE (deflate);
DEFINE_INTERPOSE (deflateEnd);
DEFINE_INTERPOSE (inflateInit_);
DEFINE_INTERPOSE (inflateInit2_);
DEFINE_INTERPOSE (inflate);
DEFINE_INTERPOSE (inflateEnd);

static void *dlsym_or_die (const char *name)
{
  void *sym;

  sym = dlsym (RTLD_NEXT, name);
  if (!sym)
    die ("could not resolve \"%s\"", name);
  return sym;
}

#define INIT_INTERPOSE(x) ORIG (x) = (typeof (&x)) dlsym_or_die (#x)

__attribute__((constructor))
static void init ()
{
  INIT_INTERPOSE (deflateInit_);
  INIT_INTERPOSE (deflateInit2_);
  INIT_INTERPOSE (deflate);
  INIT_INTERPOSE (deflateEnd);
  INIT_INTERPOSE (inflateInit_);
  INIT_INTERPOSE (inflateInit2_);
  INIT_INTERPOSE (inflate);
  INIT_INTERPOSE (inflateEnd);
}
#endif

struct call {
  struct hash_entry *stream;
  z_const Bytef *next_in;
  uInt avail_in;
  Bytef *next_out;
  uInt avail_out;
  int flush;
};

static void before_call (struct call *call, z_streamp strm, int flush) {
  call->stream = find_stream_or_die (strm);
  call->next_in = strm->next_in;
  call->avail_in = strm->avail_in;
  call->next_out = strm->next_out;
  call->avail_out = strm->avail_out;
  call->flush = flush;
}

static void after_call (struct call *call) {
  uInt consumed_in;
  uInt consumed_out;
  char buf[256];
  size_t n;

  consumed_in = call->stream->strm->next_in - call->next_in;
  write_or_die (call->stream->ifd, call->next_in, consumed_in);
  consumed_out = call->stream->strm->next_out - call->next_out;
  write_or_die (call->stream->ofd, call->next_out, consumed_out);
  n = snprintf(buf, sizeof (buf), "%p %u %p %u %i %u %u\n",
               (z_const void *) call->next_in, call->avail_in,
               (void *) call->next_out, call->avail_out,
               call->flush, consumed_in, consumed_out);
  write_or_die (call->stream->mfd, buf, n);
}

extern int REPLACEMENT (deflateInit_) (z_streamp strm, int level,
                                       const char *version, int stream_size)
{
  int err;

  err = ORIG (deflateInit_) (strm, level, version, stream_size);
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

  err = ORIG (deflateInit2_) (strm, level, method,
                              windowBits, memLevel,
                              strategy, version,
                              stream_size);
  if (err == Z_OK)
    add_stream_or_die (strm, "deflate");
  return err;
}

extern int REPLACEMENT (deflate) (z_streamp strm, int flush)
{
  struct call call;
  int err;

  before_call (&call, strm, flush);
  err = ORIG (deflate) (strm, flush);
  after_call (&call);
  return err;
}

extern int REPLACEMENT (deflateEnd) (z_streamp strm)
{
  end_stream_or_die (strm);
  return ORIG (deflateEnd) (strm);
}

extern int REPLACEMENT (inflateInit_) (z_streamp strm,
                                       const char *version, int stream_size)
{
  int err;

  err = ORIG (inflateInit_) (strm, version, stream_size);
  if (err == Z_OK)
    add_stream_or_die (strm, "inflate");
  return err;
}

extern int REPLACEMENT (inflateInit2_) (z_streamp strm, int windowBits,
                                        const char *version, int stream_size)
{
  int err;

  err = ORIG (inflateInit2_) (strm, windowBits,
                              version, stream_size);
  if (err == Z_OK)
    add_stream_or_die (strm, "inflate");
  return err;
}

extern int REPLACEMENT (inflate) (z_streamp strm, int flush)
{
  struct call call;
  int err;

  before_call (&call, strm, flush);
  err = ORIG (inflate) (strm, flush);
  after_call (&call);
  return err;
}

extern int REPLACEMENT (inflateEnd) (z_streamp strm)
{
  end_stream_or_die (strm);
  return ORIG (inflateEnd) (strm);
}

#ifdef __APPLE__
DYLD_INTERPOSE (REPLACEMENT (deflateInit_), deflateInit_)
DYLD_INTERPOSE (REPLACEMENT (deflateInit2_), deflateInit2_)
DYLD_INTERPOSE (REPLACEMENT (deflate), deflate)
DYLD_INTERPOSE (REPLACEMENT (deflateEnd), deflateEnd)
DYLD_INTERPOSE (REPLACEMENT (inflateInit_), inflateInit_)
DYLD_INTERPOSE (REPLACEMENT (inflateInit2_), inflateInit2_)
DYLD_INTERPOSE (REPLACEMENT (inflate), inflate)
DYLD_INTERPOSE (REPLACEMENT (inflateEnd), inflateEnd)
#endif
