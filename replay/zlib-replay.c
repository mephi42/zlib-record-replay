#include <memory.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <zlib.h>

#define PAGE_SIZE 0x1000
#define PAGE_OFFSET_MASK 0xfff

static int replay_init (z_streamp strm, FILE *mfp, const char *argv0)
{
  int version;
  int level;
  int method;
  int window_bits;
  int mem_level;
  int strategy;
  int err;

  memset(strm, 0, sizeof (*strm));
  if (fscanf (mfp, "%i", &version) != 1)
    {
      fprintf (stderr, "%s: could not read init version\n", argv0);
      return EXIT_FAILURE;
    }
  if (version == 1)
    {
      if (fscanf (mfp, "%i", &level) != 1)
        {
          fprintf (stderr, "%s: could not read deflateInit arguments\n", argv0);
          return EXIT_FAILURE;
        }
      err = deflateInit (strm, level);
    }
  else
    {
      if (fscanf (mfp, "%i %i %i %i %i",
                  &level, &method, &window_bits, &mem_level, &strategy) != 5)
        {
          fprintf (stderr, "%s: could not read deflateInit2 arguments\n", argv0);
          return EXIT_FAILURE;
        }
      err = deflateInit2 (strm, level, method,
                          window_bits, mem_level,
                          strategy);
    }
  return err == Z_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}

static void *align_up (void *p, int size)
{
  return (void *) (((uintptr_t) p + size - 1) & ~(size - 1));
}

static void *align_up_with_offset (void *p, int size, int offset)
{
  return (char *) align_up ((char *) p - offset, size) + offset;
}

static int fread_all (void *buf, size_t count, FILE *stream)
{
  ssize_t ret;

  while (count)
    {
      ret = fread (buf, 1, count, stream);
      if (ret <= 0)
        return -1;
      buf = (char *) buf + ret;
      count -= ret;
    }
  return 0;
}

static int replay_deflate (z_streamp strm, FILE *mfp, FILE *ifp, FILE *ofp,
                           int *eof, const char *argv0)
{
  unsigned long next_in;
  unsigned int avail_in;
  unsigned long next_out;
  unsigned int avail_out;
  int flush;
  unsigned int exp_consumed_in;
  unsigned int exp_consumed_out;
  void *buf;
  void *exp_buf;
  int err;
  unsigned int consumed_in;
  unsigned int consumed_out;
  int ret = EXIT_FAILURE;

  err = fscanf (mfp, "%lx %u %lx %u %i %u %u",
                &next_in, &avail_in, &next_out, &avail_out, &flush,
                &exp_consumed_in, &exp_consumed_out);
  if (err == EOF)
    {
      *eof = 1;
      return EXIT_SUCCESS;
    }
  if (err != 7)
    {
      fprintf (stderr, "%s: could not read deflate arguments\n", argv0);
      return EXIT_FAILURE;
    }
  buf = malloc (avail_in + PAGE_SIZE + avail_out + PAGE_SIZE + avail_out);
  if (!buf)
    {
      fprintf (stderr, "%s: oom\n", argv0);
      return EXIT_FAILURE;
    }
  strm->next_in = align_up_with_offset (
      buf, PAGE_SIZE, (int) next_in & PAGE_OFFSET_MASK);
  strm->avail_in = avail_in;
  strm->next_out = align_up_with_offset (
      strm->next_in + avail_in, PAGE_SIZE, (int) next_out & PAGE_OFFSET_MASK);
  strm->avail_out = avail_out;
  exp_buf = strm->next_out + avail_out;
  if (fread_all (strm->next_in, avail_in, ifp) == -1)
    {
      fprintf (stderr, "%s: could not read %u bytes from the input file\n",
               argv0, avail_in);
      goto free_buf;
    }
  if (fseek (ifp, (long) (int) (exp_consumed_in - avail_in), SEEK_CUR) == -1)
    {
      fprintf (stderr, "%s: could not seek in input file\n", argv0);
      goto free_buf;
    }
  if (fread_all (exp_buf, avail_out, ofp) == -1)
    {
      fprintf (stderr, "%s: could not read %u bytes from the output file\n",
               argv0, avail_out);
      goto free_buf;
    }
  if (fseek (ofp, (long) (int) (exp_consumed_out - avail_out), SEEK_CUR) == -1)
    {
      fprintf (stderr, "%s: could not seek in output file\n", argv0);
      goto free_buf;
    }
  err = deflate (strm, flush);
  consumed_in = avail_in - strm->avail_in;
  consumed_out = avail_out - strm->avail_out;
  if (err != Z_OK && err != Z_STREAM_END)
    fprintf (stderr, "%s: deflate failed\n", argv0);
  else if (consumed_in != exp_consumed_in)
    fprintf (stderr, "%s: consumed_in mismatch (%u vs %u)\n",
             argv0, consumed_in, exp_consumed_in);
  else if (consumed_out != exp_consumed_out)
    fprintf (stderr, "%s: consumed_out mismatch (%u vs %u)\n",
             argv0, consumed_out, exp_consumed_out);
  else if (memcmp (strm->next_out - consumed_out, exp_buf, consumed_out) != 0)
    fprintf (stderr, "%s: compressed data mismatch\n", argv0);
  else
    {
      fprintf (stderr, "%s: ok\n", argv0);
      ret = EXIT_SUCCESS;
    }
free_buf:
  free (buf);
  return ret;
}

int main (int argc, char **argv)
{
  FILE *mfp;
  char path[256];
  FILE *ifp;
  FILE *ofp;
  z_stream strm;
  int eof = 0;
  int ret = EXIT_FAILURE;

  if (argc != 2)
    {
      fprintf (stderr, "Usage: %s deflate.X.Y\n", argv[0]);
      goto done;
    }
  if (!(mfp = fopen (argv[1], "r")))
    {
      fprintf (stderr, "%s: could not open %s\n", argv[0], argv[1]);
      goto done;
    }
  path[snprintf(path, sizeof (path) - 1, "%s.in", argv[1])] = 0;
  if (!(ifp = fopen (path, "r")))
    {
      fprintf (stderr, "%s: could not open %s\n", argv[0], path);
      goto close_mfp;
    }
  path[snprintf(path, sizeof (path) - 1, "%s.out", argv[1])] = 0;
  if (!(ofp = fopen (path, "r")))
    {
      fprintf (stderr, "%s: could not open %s\n", argv[0], path);
      goto close_ifp;
    }
  if (replay_init (&strm, mfp, argv[0]) != EXIT_SUCCESS)
    {
      fprintf (stderr, "%s: init failed\n", argv[0]);
      goto close_ofp;
    }
  while (!eof)
    {
      if (replay_deflate (&strm, mfp, ifp, ofp, &eof, argv[0]) != EXIT_SUCCESS)
        {
          fprintf (stderr, "%s: deflate failed at offset "
                           "uncompressed:%lu compressed:%lu\n",
                   argv[0], strm.total_in, strm.total_out);
          goto close_ofp;
        }
    }
  ret = EXIT_SUCCESS;
close_ofp:
  fclose (ofp);
close_ifp:
  fclose (ifp);
close_mfp:
  fclose (mfp);
done:
  return ret;
}
