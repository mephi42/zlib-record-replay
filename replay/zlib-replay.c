#include <memory.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <zlib.h>

struct replay_state
{
  FILE *mfp;
  FILE *ifp;
  FILE *ofp;
  z_stream strm;
  char kind;
};

static int replay_run (struct replay_state *replay, const char *path,
                       unsigned long end_off, const char *argv0);
static int replay_end (struct replay_state *replay);

#define PAGE_SIZE 0x1000
#define PAGE_OFFSET_MASK 0xfff

static const char *
stream_kind (char kind)
{
  return kind == 'd' ? "deflate" : "inflate";
}

static int
replay_copy (struct replay_state *replay, int *z_err, const char *argv0)
{
  char source_path[256];
  unsigned long source_off;
  struct replay_state replay_source;
  int err;

  if (fscanf (replay->mfp, "%256s %lu", source_path, &source_off) != 2)
    {
      fprintf (stderr, "%s: could not read %sCopy arguments\n", argv0,
               stream_kind (replay->kind));
      return EXIT_FAILURE;
    }
  err = replay_run (&replay_source, source_path, source_off, argv0);
  if (err != EXIT_SUCCESS)
    {
      fprintf (stderr, "%s: run %s failed\n", argv0, source_path);
      return EXIT_FAILURE;
    }
  *z_err = replay->kind == 'd'
               ? deflateCopy (&replay->strm, &replay_source.strm)
               : inflateCopy (&replay->strm, &replay_source.strm);
  replay_end (&replay_source); /* ignore rc */
  return EXIT_SUCCESS;
}

static int
replay_init (struct replay_state *replay, const char *argv0)
{
  char init_method[2];
  int level;
  int method;
  int window_bits;
  int mem_level;
  int strategy;
  int err;

  memset (&replay->strm, 0, sizeof (replay->strm));
  if (fscanf (replay->mfp, "%c", &replay->kind) != 1)
    {
      fprintf (stderr, "%s: could not read stream type\n", argv0);
      return EXIT_FAILURE;
    }
  if (fscanf (replay->mfp, "%1s", init_method) != 1)
    {
      fprintf (stderr, "%s: could not read init method\n", argv0);
      return EXIT_FAILURE;
    }
  if (replay->kind == 'd' && init_method[0] == '1')
    {
      if (fscanf (replay->mfp, "%i", &level) != 1)
        {
          fprintf (stderr, "%s: could not read deflateInit arguments\n",
                   argv0);
          return EXIT_FAILURE;
        }
      err = deflateInit (&replay->strm, level);
    }
  else if (replay->kind == 'd' && init_method[0] == '2')
    {
      if (fscanf (replay->mfp, "%i %i %i %i %i", &level, &method, &window_bits,
                  &mem_level, &strategy)
          != 5)
        {
          fprintf (stderr, "%s: could not read deflateInit2 arguments\n",
                   argv0);
          return EXIT_FAILURE;
        }
      err = deflateInit2 (&replay->strm, level, method, window_bits, mem_level,
                          strategy);
    }
  else if (replay->kind == 'd' && init_method[0] == 'c')
    {
      if (replay_copy (replay, &err, argv0) != EXIT_SUCCESS)
        return EXIT_FAILURE;
    }
  else if (replay->kind == 'i' && init_method[0] == '1')
    {
      err = inflateInit (&replay->strm);
    }
  else if (replay->kind == 'i' && init_method[0] == '2')
    {
      if (fscanf (replay->mfp, "%i", &window_bits) != 1)
        {
          fprintf (stderr, "%s: could not read inflateInit2 argument\n",
                   argv0);
          return EXIT_FAILURE;
        }
      err = inflateInit2 (&replay->strm, window_bits);
    }
  else if (replay->kind == 'i' && init_method[0] == 'c')
    {
      if (replay_copy (replay, &err, argv0) != EXIT_SUCCESS)
        return EXIT_FAILURE;
    }
  else
    {
      fprintf (stderr, "%s: unsupported stream kind and init method\n", argv0);
      err = Z_STREAM_ERROR;
    }
  return err == Z_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}

static void *
align_up (void *p, int size)
{
  return (void *)(((uintptr_t)p + size - 1) & ~(size - 1));
}

static void *
align_up_with_offset (void *p, int size, int offset)
{
  return (char *)align_up ((char *)p - offset, size) + offset;
}

static ssize_t
fread_all (void *buf, size_t count, FILE *stream)
{
  ssize_t ret;
  size_t n = count;

  while (n)
    {
      ret = fread (buf, 1, n, stream);
      if (ret == 0)
        {
          if (feof (stream))
            break;
          else
            return -1;
        }
      buf = (char *)buf + ret;
      n -= ret;
    }
  return count - n;
}

static int
replay_one (struct replay_state *replay, int *eof, const char *argv0)
{
  char call_kind[2];
  unsigned long next_in;
  unsigned int avail_in;
  unsigned long next_out;
  unsigned int avail_out;
  const char *func;
  int level;
  int strategy;
  int flush;
  unsigned int exp_consumed_in;
  unsigned int exp_consumed_out;
  int exp_err;
  long fseek_offset;
  void *buf;
  void *exp_buf;
  ssize_t valid_in;
  ssize_t valid_out;
  int err;
  int z_err;
  unsigned int consumed_in;
  unsigned int consumed_out;
  Bytef *actual_out;
  int ret = EXIT_FAILURE;

  err = fscanf (replay->mfp, "%1s", call_kind);
  if (err == EOF)
    {
      *eof = 1;
      return EXIT_SUCCESS;
    }
  if (err != 1)
    {
      fprintf (stderr, "%s: could not read call kind\n", argv0);
      return EXIT_FAILURE;
    }
  switch (call_kind[0])
    {
    case 'p':
      func = "deflateParams";
      err = fscanf (replay->mfp, "%i %i", &level, &strategy);
      if (err != 2)
        {
          fprintf (stderr, "%s: could not read %s arguments\n", argv0, func);
          return EXIT_FAILURE;
        }
      break;
    case 'c':
      func = stream_kind (replay->kind);
      err = fscanf (replay->mfp, "%i", &flush);
      if (err != 1)
        {
          fprintf (stderr, "%s: could not read %s arguments\n", argv0, func);
          return EXIT_FAILURE;
        }
      break;
    case 'r':
      func = replay->kind == 'd' ? "deflateReset" : "inflateReset";
      break;
    default:
      fprintf (stderr, "%s: unsupported call kind\n", argv0);
      return EXIT_FAILURE;
    }
  err = fscanf (replay->mfp, "%lx %u %lx %u", &next_in, &avail_in, &next_out,
                &avail_out);
  if (err != 4)
    {
      fprintf (stderr, "%s: could not read stream pointers\n", argv0);
      return EXIT_FAILURE;
    }
  buf = malloc (avail_in + PAGE_SIZE + avail_out + PAGE_SIZE + avail_out);
  if (!buf)
    {
      fprintf (stderr, "%s: oom\n", argv0);
      return EXIT_FAILURE;
    }
  replay->strm.next_in
      = align_up_with_offset (buf, PAGE_SIZE, (int)next_in & PAGE_OFFSET_MASK);
  replay->strm.avail_in = avail_in;
  replay->strm.next_out
      = align_up_with_offset (replay->strm.next_in + avail_in, PAGE_SIZE,
                              (int)next_out & PAGE_OFFSET_MASK);
  replay->strm.avail_out = avail_out;
  exp_buf = replay->strm.next_out + avail_out;
  valid_in = fread_all (replay->strm.next_in, avail_in, replay->ifp);
  if (valid_in == -1)
    {
      fprintf (stderr, "%s: could not read %u bytes from the input file\n",
               argv0, avail_in);
      goto free_buf;
    }
  valid_out = fread_all (exp_buf, avail_out, replay->ofp);
  if (valid_out == -1)
    {
      fprintf (stderr, "%s: could not read %u bytes from the output file\n",
               argv0, avail_out);
      goto free_buf;
    }
  switch (call_kind[0])
    {
    case 'p':
      z_err = deflateParams (&replay->strm, level, strategy);
      break;
    case 'c':
      z_err = replay->kind == 'd' ? deflate (&replay->strm, flush)
                                  : inflate (&replay->strm, flush);
      break;
    case 'r':
      z_err = replay->kind == 'd' ? deflateReset (&replay->strm)
                                  : inflateReset (&replay->strm);
      break;
    }
  err = fscanf (replay->mfp, "%u %u %i", &exp_consumed_in, &exp_consumed_out,
                &exp_err);
  if (err != 3)
    {
      fprintf (stderr, "%s: could not read %s results\n", argv0, func);
      return EXIT_FAILURE;
    }
  fseek_offset = (long)(int)(exp_consumed_in - valid_in);
  if (fseek (replay->ifp, fseek_offset, SEEK_CUR) == -1)
    {
      fprintf (stderr, "%s: could not seek by %ld in the input file\n", argv0,
               fseek_offset);
      goto free_buf;
    }
  fseek_offset = (long)(int)(exp_consumed_out - valid_out);
  if (fseek (replay->ofp, fseek_offset, SEEK_CUR) == -1)
    {
      fprintf (stderr, "%s: could not seek by %ld in the output file\n", argv0,
               fseek_offset);
      goto free_buf;
    }
  consumed_in = avail_in - replay->strm.avail_in;
  consumed_out = avail_out - replay->strm.avail_out;
  actual_out = replay->strm.next_out - consumed_out;
  if (z_err != exp_err)
    fprintf (stderr,
             "%s: %s return value mismatch (actual: %i, expected: %i)\n",
             argv0, func, z_err, exp_err);
  else if (consumed_in != exp_consumed_in)
    fprintf (stderr, "%s: consumed_in mismatch (actual: %u, expected: %u)\n",
             argv0, consumed_in, exp_consumed_in);
  else if (consumed_out != exp_consumed_out)
    fprintf (stderr, "%s: consumed_out mismatch (actual: %u expected:%u)\n",
             argv0, consumed_out, exp_consumed_out);
  else if (memcmp (actual_out, exp_buf, consumed_out) != 0)
    fprintf (stderr, "%s: %scompressed data mismatch\n", argv0,
             replay->kind == 'd' ? "" : "un");
  else
    ret = EXIT_SUCCESS;
free_buf:
  free (buf);
  return ret;
}

static int
replay_open (struct replay_state *replay, const char *path, const char *argv0)
{
  char buf[256];

  if (!(replay->mfp = fopen (path, "r")))
    {
      fprintf (stderr, "%s: could not open %s\n", argv0, path);
      goto fail;
    }
  buf[snprintf (buf, sizeof (buf) - 1, "%s.in", path)] = 0;
  if (!(replay->ifp = fopen (buf, "r")))
    {
      fprintf (stderr, "%s: could not open %s\n", argv0, buf);
      goto fail_close_mfp;
    }
  buf[snprintf (buf, sizeof (buf) - 1, "%s.out", path)] = 0;
  if (!(replay->ofp = fopen (buf, "r")))
    {
      fprintf (stderr, "%s: could not open %s\n", argv0, buf);
      goto fail_close_ifp;
    }
  return EXIT_SUCCESS;
fail_close_ifp:
  fclose (replay->ifp);
fail_close_mfp:
  fclose (replay->mfp);
fail:
  return EXIT_FAILURE;
}

static void
replay_close (struct replay_state *replay)
{
  fclose (replay->ofp);
  fclose (replay->ifp);
  fclose (replay->mfp);
}

static int
replay_run (struct replay_state *replay, const char *path,
            unsigned long end_off, const char *argv0)
{
  int eof = 0;
  unsigned long off;
  int ret = EXIT_FAILURE;

  if (replay_open (replay, path, argv0) != EXIT_SUCCESS)
    {
      fprintf (stderr, "%s: open failed\n", argv0);
      goto done;
    }
  if (replay_init (replay, argv0) != EXIT_SUCCESS)
    {
      fprintf (stderr, "%s: init failed\n", argv0);
      goto close_replay;
    }
  while (!eof)
    {
      off = (unsigned long)ftell (replay->mfp);
      if (off == -1UL)
        {
          fprintf (stderr, "%s: ftell() failed\n", argv0);
          goto close_replay;
        }
      if (off >= end_off)
        break;
      if (replay_one (replay, &eof, argv0) != EXIT_SUCCESS)
        {
          fprintf (stderr,
                   "%s: %s failed at offset "
                   "uncompressed:%lu compressed:%lu\n",
                   argv0, stream_kind (replay->kind), replay->strm.total_in,
                   replay->strm.total_out);
          goto close_replay;
        }
    }
  ret = EXIT_SUCCESS;
close_replay:
  replay_close (replay);
done:
  return ret;
}

static int
replay_end (struct replay_state *replay)
{
  return replay->kind == 'd' ? deflateEnd (&replay->strm)
                             : inflateEnd (&replay->strm);
}

int
main (int argc, char **argv)
{
  struct replay_state replay;
  int ret = EXIT_FAILURE;

  if (argc != 2)
    {
      fprintf (stderr, "Usage: %s {deflate | inflate}.PID.STREAM\n", argv[0]);
      goto done;
    }
  if (replay_run (&replay, argv[1], -1UL, argv[0]) != EXIT_SUCCESS)
    {
      fprintf (stderr, "%s: run %s failed\n", argv[0], argv[1]);
      goto done;
    }
  if (replay_end (&replay) != Z_OK)
    {
      fprintf (stderr, "%s: %sEnd %s failed\n", argv[0],
               stream_kind (replay.kind), argv[1]);
      goto done;
    }
  ret = EXIT_SUCCESS;
done:
  return ret;
}
