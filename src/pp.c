#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#define __USE_MINGW_ANSI_STDIO 1

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <getopt.h>
#include <gmp.h>

/**
 * Name........: princeprocessor (pp)
 * Description.: Standalone password candidate generator using the PRINCE algorithm
 * Version.....: 0.19
 * Autor.......: Jens Steube <jens.steube@gmail.com>
 * License.....: MIT
 */

#define IN_LEN_MIN    1
#define IN_LEN_MAX    16
#define PW_MIN        IN_LEN_MIN
#define PW_MAX        IN_LEN_MAX
#define ELEM_CNT_MIN  1
#define ELEM_CNT_MAX  8
#define WL_DIST_LEN   0

#define VERSION_BIN   19

#define ALLOC_NEW_ELEMS  0x40000
#define ALLOC_NEW_CHAINS 0x10

#define MIN(a,b) (((a) < (b)) ? (a) : (b))
#define MAX(a,b) (((a) > (b)) ? (a) : (b))

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef struct
{
  int len;
  u64 cnt;

} pw_order_t;

typedef struct
{
  u8    buf[IN_LEN_MAX];

} elem_t;

typedef struct
{
  u8    buf[IN_LEN_MAX];
  int   cnt;

  mpz_t ks_cnt;
  mpz_t ks_pos;

} chain_t;

typedef struct
{
  elem_t  *elems_buf;
  u64      elems_cnt;
  u64      elems_alloc;

  chain_t *chains_buf;
  int      chains_cnt;
  int      chains_pos;
  int      chains_alloc;

  u64      cur_chain_ks_poses[ELEM_CNT_MAX];

} db_entry_t;

typedef struct
{
  FILE *fp;

  char  buf[BUFSIZ];
  int   len;

} out_t;

/**
 * Default word-length distribution, calculated out of first 1,000,000 entries of rockyou.txt
 */

#define DEF_WORDLEN_DIST_CNT 25

static u64 DEF_WORDLEN_DIST[DEF_WORDLEN_DIST_CNT] =
{
  0,
  15,
  56,
  350,
  3315,
  43721,
  276252,
  201748,
  226412,
  119885,
  75075,
  26323,
  13373,
  6353,
  3540,
  1877,
  972,
  311,
  151,
  81,
  66,
  21,
  16,
  13,
  13
};

static const char *USAGE_MINI[] =
{
  "Usage: %s [options] < wordlist",
  "",
  "Try --help for more help.",
  NULL
};

static const char *USAGE_BIG[] =
{
  "pp by atom, High-Performance word-generator",
  "",
  "Usage: %s [options] < wordlist",
  "",
  "* Startup:",
  "",
  "  -V,  --version             Print version",
  "  -h,  --help                Print help",
  "",
  "* Misc:",
  "",
  "       --keyspace            Calculate number of combinations",
  "",
  "* Optimization:",
  "",
  "       --pw-min=NUM          Print candidate if length is greater than NUM",
  "       --pw-max=NUM          Print candidate if length is smaller than NUM",
  "       --elem-cnt-min=NUM    Minimum number of elements per chain",
  "       --elem-cnt-max=NUM    Maximum number of elements per chain",
  "       --wl-dist-len         Calculate output length distribution from wordlist",
  "",
  "* Resources:",
  "",
  "  -s,  --skip=NUM            Skip NUM passwords from start (for distributed)",
  "  -l,  --limit=NUM           Limit output to NUM passwords (for distributed)",
  "",
  "* Files:",
  "",
  "  -o,  --output-file=FILE    Output-file",
  "",
  NULL
};

static void usage_mini_print (const char *progname)
{
  int i;

  for (i = 0; USAGE_MINI[i] != NULL; i++)
  {
    printf (USAGE_MINI[i], progname);

    #ifdef OSX
    putchar ('\n');
    #endif

    #ifdef LINUX
    putchar ('\n');
    #endif

    #ifdef WINDOWS
    putchar ('\r');
    putchar ('\n');
    #endif
  }
}

static void usage_big_print (const char *progname)
{
  int i;

  for (i = 0; USAGE_BIG[i] != NULL; i++)
  {
    printf (USAGE_BIG[i], progname);

    #ifdef OSX
    putchar ('\n');
    #endif

    #ifdef LINUX
    putchar ('\n');
    #endif

    #ifdef WINDOWS
    putchar ('\r');
    putchar ('\n');
    #endif
  }
}

static void check_realloc_elems (db_entry_t *db_entry)
{
  if (db_entry->elems_cnt == db_entry->elems_alloc)
  {
    const u64 elems_alloc = db_entry->elems_alloc;

    const u64 elems_alloc_new = elems_alloc + ALLOC_NEW_ELEMS;

    db_entry->elems_buf = (elem_t *) realloc (db_entry->elems_buf, elems_alloc_new * sizeof (elem_t));

    if (db_entry->elems_buf == NULL)
    {
      fprintf (stderr, "Out of memory trying to allocate %zu bytes!\n",
               (size_t)elems_alloc_new * sizeof (elem_t));

      exit (-1);
    }

    memset (&db_entry->elems_buf[elems_alloc], 0, ALLOC_NEW_ELEMS * sizeof (elem_t));

    db_entry->elems_alloc = elems_alloc_new;
  }
}

static void check_realloc_chains (db_entry_t *db_entry)
{
  if (db_entry->chains_cnt == db_entry->chains_alloc)
  {
    const u64 chains_alloc = db_entry->chains_alloc;

    const u64 chains_alloc_new = chains_alloc + ALLOC_NEW_CHAINS;

    db_entry->chains_buf = (chain_t *) realloc (db_entry->chains_buf, chains_alloc_new * sizeof (chain_t));

    if (db_entry->chains_buf == NULL)
    {
      fprintf (stderr, "Out of memory trying to allocate %zu bytes!\n",
               (size_t)chains_alloc_new * sizeof (chain_t));

      exit (-1);
    }

    memset (&db_entry->chains_buf[chains_alloc], 0, ALLOC_NEW_CHAINS * sizeof (chain_t));

    db_entry->chains_alloc = chains_alloc_new;
  }
}

static int in_superchop (char *buf)
{
  int len = strlen (buf);

  while (len)
  {
    if (buf[len - 1] == '\n')
    {
      len--;

      continue;
    }

    if (buf[len - 1] == '\r')
    {
      len--;

      continue;
    }

    break;
  }

  buf[len] = 0;

  return len;
}

static void out_flush (out_t *out)
{
  fwrite (out->buf, 1, out->len, out->fp);

  out->len = 0;
}

static void out_push (out_t *out, const char *pw_buf, const int pw_len)
{
  memcpy (out->buf + out->len, pw_buf, pw_len);

  out->len += pw_len;

  if (out->len >= BUFSIZ - 100)
  {
    out_flush (out);
  }
}

static int sort_by_cnt (const void *p1, const void *p2)
{
  const pw_order_t *o1 = (const pw_order_t *) p1;
  const pw_order_t *o2 = (const pw_order_t *) p2;

  // Descending order
  if (o1->cnt > o2->cnt) return -1;
  if (o1->cnt < o2->cnt) return  1;

  return 0;
}

static int sort_by_ks (const void *p1, const void *p2)
{
  const chain_t *f1 = (const chain_t *) p1;
  const chain_t *f2 = (const chain_t *) p2;

  return mpz_cmp (f1->ks_cnt, f2->ks_cnt);
}

static int chain_valid_with_db (const chain_t *chain_buf, const db_entry_t *db_entries)
{
  const u8 *buf = chain_buf->buf;
  const int cnt = chain_buf->cnt;

  for (int idx = 0; idx < cnt; idx++)
  {
    const u8 db_key = buf[idx];

    const db_entry_t *db_entry = &db_entries[db_key];

    if (db_entry->elems_cnt == 0) return 0;
  }

  return 1;
}

static int chain_valid_with_cnt_min (const chain_t *chain_buf, const int elem_cnt_min)
{
  const int cnt = chain_buf->cnt;

  if (cnt < elem_cnt_min) return 0;

  return 1;
}

static int chain_valid_with_cnt_max (const chain_t *chain_buf, const int elem_cnt_max)
{
  const int cnt = chain_buf->cnt;

  if (cnt > elem_cnt_max) return 0;

  return 1;
}

static void chain_ks (const chain_t *chain_buf, const db_entry_t *db_entries, mpz_t ks_cnt)
{
  const u8 *buf = chain_buf->buf;
  const int cnt = chain_buf->cnt;

  mpz_set_si (ks_cnt, 1);

  for (int idx = 0; idx < cnt; idx++)
  {
    const u8 db_key = buf[idx];

    const db_entry_t *db_entry = &db_entries[db_key];

    const u64 elems_cnt = db_entry->elems_cnt;

    mpz_mul_ui (ks_cnt, ks_cnt, elems_cnt);
  }
}

static void set_chain_ks_poses (const chain_t *chain_buf, const db_entry_t *db_entries, mpz_t tmp, u64 cur_chain_ks_poses[ELEM_CNT_MAX])
{
  const u8 *buf = chain_buf->buf;

  const int cnt = chain_buf->cnt;

  for (int idx = 0; idx < cnt; idx++)
  {
    const u8 db_key = buf[idx];

    const db_entry_t *db_entry = &db_entries[db_key];

    const u64 elems_cnt = db_entry->elems_cnt;

    cur_chain_ks_poses[idx] = mpz_fdiv_ui (tmp, elems_cnt);

    mpz_div_ui (tmp, tmp, elems_cnt);
  }
}

static void chain_set_pwbuf_init (const chain_t *chain_buf, const db_entry_t *db_entries, const u64 cur_chain_ks_poses[ELEM_CNT_MAX], char *pw_buf)
{
  const u8 *buf = chain_buf->buf;

  const u32 cnt = chain_buf->cnt;

  for (u32 idx = 0; idx < cnt; idx++)
  {
    const u8 db_key = buf[idx];

    const db_entry_t *db_entry = &db_entries[db_key];

    const u64 elems_idx = cur_chain_ks_poses[idx];

    memcpy (pw_buf, &db_entry->elems_buf[elems_idx], db_key);

    pw_buf += db_key;
  }
}

static void chain_set_pwbuf_increment (const chain_t *chain_buf, const db_entry_t *db_entries, u64 cur_chain_ks_poses[ELEM_CNT_MAX], char *pw_buf)
{
  const u8 *buf = chain_buf->buf;

  const int cnt = chain_buf->cnt;

  for (int idx = 0; idx < cnt; idx++)
  {
    const u8 db_key = buf[idx];

    const db_entry_t *db_entry = &db_entries[db_key];

    const u64 elems_cnt = db_entry->elems_cnt;

    const u64 elems_idx = ++cur_chain_ks_poses[idx];

    if (elems_idx < elems_cnt)
    {
      memcpy (pw_buf, &db_entry->elems_buf[elems_idx], db_key);

      break;
    }

    cur_chain_ks_poses[idx] = 0;

    memcpy (pw_buf, &db_entry->elems_buf[0], db_key);

    pw_buf += db_key;
  }
}

static void chain_gen_with_idx (chain_t *chain_buf, const int len1, const int chains_idx)
{
  chain_buf->cnt = 0;

  u8 db_key = 1;

  for (int chains_shr = 0; chains_shr < len1; chains_shr++)
  {
    if ((chains_idx >> chains_shr) & 1)
    {
      chain_buf->buf[chain_buf->cnt] = db_key;

      chain_buf->cnt++;

      db_key = 1;
    }
    else
    {
      db_key++;
    }
  }

  chain_buf->buf[chain_buf->cnt] = db_key;

  chain_buf->cnt++;
}

int main (int argc, char *argv[])
{
  mpz_t pw_ks_pos[PW_MAX + 1];
  mpz_t pw_ks_cnt[PW_MAX + 1];

  mpz_t iter_max;         mpz_init_set_si (iter_max,        0);
  mpz_t total_ks_cnt;     mpz_init_set_si (total_ks_cnt,    0);
  mpz_t total_ks_pos;     mpz_init_set_si (total_ks_pos,    0);
  mpz_t total_ks_left;    mpz_init_set_si (total_ks_left,   0);
  mpz_t skip;             mpz_init_set_si (skip,            0);
  mpz_t limit;            mpz_init_set_si (limit,           0);
  mpz_t tmp;              mpz_init_set_si (tmp,             0);

  int     version       = 0;
  int     usage         = 0;
  int     keyspace      = 0;
  int     pw_min        = PW_MIN;
  int     pw_max        = PW_MAX;
  int     elem_cnt_min  = ELEM_CNT_MIN;
  int     elem_cnt_max  = ELEM_CNT_MAX;
  int     wl_dist_len   = WL_DIST_LEN;
  char   *output_file   = NULL;

  #define IDX_VERSION       'V'
  #define IDX_USAGE         'h'
  #define IDX_PW_MIN        0x1000
  #define IDX_PW_MAX        0x2000
  #define IDX_ELEM_CNT_MIN  0x3000
  #define IDX_ELEM_CNT_MAX  0x4000
  #define IDX_KEYSPACE      0x5000
  #define IDX_WL_DIST_LEN   0x6000
  #define IDX_SKIP          's'
  #define IDX_LIMIT         'l'
  #define IDX_OUTPUT_FILE   'o'

  struct option long_options[] =
  {
    {"version",       no_argument,       0, IDX_VERSION},
    {"help",          no_argument,       0, IDX_USAGE},
    {"keyspace",      no_argument,       0, IDX_KEYSPACE},
    {"pw-min",        required_argument, 0, IDX_PW_MIN},
    {"pw-max",        required_argument, 0, IDX_PW_MAX},
    {"elem-cnt-min",  required_argument, 0, IDX_ELEM_CNT_MIN},
    {"elem-cnt-max",  required_argument, 0, IDX_ELEM_CNT_MAX},
    {"wl-dist-len",   no_argument,       0, IDX_WL_DIST_LEN},
    {"skip",          required_argument, 0, IDX_SKIP},
    {"limit",         required_argument, 0, IDX_LIMIT},
    {"output-file",   required_argument, 0, IDX_OUTPUT_FILE},
    {0, 0, 0, 0}
  };

  int option_index = 0;

  int c;

  while ((c = getopt_long (argc, argv, "Vhs:l:o:", long_options, &option_index)) != -1)
  {
    switch (c)
    {
      case IDX_VERSION:       version         = 1;              break;
      case IDX_USAGE:         usage           = 1;              break;
      case IDX_KEYSPACE:      keyspace        = 1;              break;
      case IDX_PW_MIN:        pw_min          = atoi (optarg);  break;
      case IDX_PW_MAX:        pw_max          = atoi (optarg);  break;
      case IDX_ELEM_CNT_MIN:  elem_cnt_min    = atoi (optarg);  break;
      case IDX_ELEM_CNT_MAX:  elem_cnt_max    = atoi (optarg);  break;
      case IDX_WL_DIST_LEN:   wl_dist_len     = 1;              break;
      case IDX_SKIP:          mpz_set_str (skip,  optarg, 0);   break;
      case IDX_LIMIT:         mpz_set_str (limit, optarg, 0);   break;
      case IDX_OUTPUT_FILE:   output_file     = optarg;         break;

      default: return (-1);
    }
  }

  if (usage)
  {
    usage_big_print (argv[0]);

    return (-1);
  }

  if (version)
  {
    printf ("v%4.02f\n", (double) VERSION_BIN / 100);

    return (-1);
  }

  if (optind != argc)
  {
    usage_mini_print (argv[0]);

    return (-1);
  }

  if (pw_min <= 0)
  {
    fprintf (stderr, "Value of --pw-min (%d) must be greater than %d\n", pw_min, 0);

    return (-1);
  }

  if (pw_max <= 0)
  {
    fprintf (stderr, "Value of --pw-max (%d) must be greater than %d\n", pw_max, 0);

    return (-1);
  }

  if (elem_cnt_min <= 0)
  {
    fprintf (stderr, "Value of --elem-cnt-min (%d) must be greater than %d\n", elem_cnt_min, 0);

    return (-1);
  }

  if (elem_cnt_max <= 0)
  {
    fprintf (stderr, "Value of --elem-cnt-max (%d) must be greater than %d\n", elem_cnt_max, 0);

    return (-1);
  }

  if (pw_min > pw_max)
  {
    fprintf (stderr, "Value of --pw-min (%d) must be smaller or equal than value of --pw-max (%d)\n", pw_min, pw_max);

    return (-1);
  }

  if (elem_cnt_min > elem_cnt_max)
  {
    fprintf (stderr, "Value of --elem-cnt-min (%d) must be smaller or equal than value of --elem-cnt-max (%d)\n", elem_cnt_min, elem_cnt_max);

    return (-1);
  }

  if (pw_min < IN_LEN_MIN)
  {
    fprintf (stderr, "Value of --pw-min (%d) must be greater or equal than %d\n", pw_min, IN_LEN_MIN);

    return (-1);
  }

  if (pw_max > IN_LEN_MAX)
  {
    fprintf (stderr, "Value of --pw-max (%d) must be smaller or equal than %d\n", pw_max, IN_LEN_MAX);

    return (-1);
  }

  if (elem_cnt_max > pw_max)
  {
    fprintf (stderr, "Value of --elem-cnt-max (%d) must be smaller or equal than value of --pw-max (%d)\n", elem_cnt_max, pw_max);

    return (-1);
  }

  /**
   * OS specific settings
   */

  #ifdef WINDOWS
  setmode (fileno (stdout), O_BINARY);
  #endif

  /**
   * alloc some space
   */

  db_entry_t *db_entries   = (db_entry_t *) calloc (IN_LEN_MAX + 1, sizeof (db_entry_t));
  pw_order_t *pw_orders    = (pw_order_t *) calloc (IN_LEN_MAX + 1, sizeof (pw_order_t));
  u64        *wordlen_dist = (u64 *)        calloc (IN_LEN_MAX + 1, sizeof (u64));

  out_t *out = (out_t *) malloc (sizeof (out_t));

  out->fp  = stdout;
  out->len = 0;

  /**
   * files
   */

  if (output_file)
  {
    out->fp = fopen (output_file, "ab");

    if (out->fp == NULL)
    {
      fprintf (stderr, "%s: %s\n", output_file, strerror (errno));

      return (-1);
    }
  }

  /**
   * load elems from stdin
   */

  while (!feof (stdin))
  {
    char buf[BUFSIZ];

    char *input_buf = fgets (buf, sizeof (buf), stdin);

    if (input_buf == NULL) continue;

    const int input_len = in_superchop (input_buf);

    if (input_len < IN_LEN_MIN) continue;
    if (input_len > IN_LEN_MAX) continue;

    db_entry_t *db_entry = &db_entries[input_len];

    check_realloc_elems (db_entry);

    elem_t *elem_buf = &db_entry->elems_buf[db_entry->elems_cnt];

    memcpy (elem_buf->buf, input_buf, input_len);

    db_entry->elems_cnt++;
  }

  /**
   * init chains
   */

  for (int pw_len = pw_min; pw_len <= pw_max; pw_len++)
  {
    db_entry_t *db_entry = &db_entries[pw_len];

    const int pw_len1 = pw_len - 1;

    const int chains_cnt = 1 << pw_len1;

    chain_t chain_buf_new;

    for (int chains_idx = 0; chains_idx < chains_cnt; chains_idx++)
    {
      chain_gen_with_idx (&chain_buf_new, pw_len1, chains_idx);

      // make sure all the elements really exist

      int valid1 = chain_valid_with_db (&chain_buf_new, db_entries);

      if (valid1 == 0) continue;

      // boost by verify element count to be inside a specific range

      int valid2 = chain_valid_with_cnt_min (&chain_buf_new, elem_cnt_min);

      if (valid2 == 0) continue;

      int valid3 = chain_valid_with_cnt_max (&chain_buf_new, elem_cnt_max);

      if (valid3 == 0) continue;

      // add chain to database

      check_realloc_chains (db_entry);

      chain_t *chain_buf = &db_entry->chains_buf[db_entry->chains_cnt];

      memcpy (chain_buf, &chain_buf_new, sizeof (chain_t));

      mpz_init_set_si (chain_buf->ks_cnt, 0);
      mpz_init_set_si (chain_buf->ks_pos, 0);

      db_entry->chains_cnt++;
    }

    memset (db_entry->cur_chain_ks_poses, 0, ELEM_CNT_MAX * sizeof (u64));
  }

  /**
   * calculate password candidate output length distribution
   */

  if (wl_dist_len)
  {
    for (int pw_len = IN_LEN_MIN; pw_len <= IN_LEN_MAX; pw_len++)
    {
      db_entry_t *db_entry = &db_entries[pw_len];

      wordlen_dist[pw_len] = db_entry->elems_cnt;
    }
  }
  else
  {
    for (int pw_len = IN_LEN_MIN; pw_len <= IN_LEN_MAX; pw_len++)
    {
      if (pw_len < DEF_WORDLEN_DIST_CNT)
      {
        wordlen_dist[pw_len] = DEF_WORDLEN_DIST[pw_len];
      }
      else
      {
        wordlen_dist[pw_len] = 1;
      }
    }
  }

  /**
   * Calculate keyspace stuff
   */

  for (int pw_len = pw_min; pw_len <= pw_max; pw_len++)
  {
    db_entry_t *db_entry = &db_entries[pw_len];

    int      chains_cnt = db_entry->chains_cnt;
    chain_t *chains_buf = db_entry->chains_buf;

    mpz_set_si (tmp, 0);

    for (int chains_idx = 0; chains_idx < chains_cnt; chains_idx++)
    {
      chain_t *chain_buf = &chains_buf[chains_idx];

      chain_ks (chain_buf, db_entries, chain_buf->ks_cnt);

      mpz_add (tmp, tmp, chain_buf->ks_cnt);
    }

    mpz_add (total_ks_cnt, total_ks_cnt, tmp);

    if (mpz_cmp_si (skip, 0))
    {
      mpz_init_set (pw_ks_cnt[pw_len], tmp);
    }
  }

  if (keyspace)
  {
    mpz_out_str (stdout, 10, total_ks_cnt);

    printf ("\n");

    return 0;
  }

  /**
   * sort chains by ks
   */

  for (int pw_len = pw_min; pw_len <= pw_max; pw_len++)
  {
    db_entry_t *db_entry = &db_entries[pw_len];

    chain_t *chains_buf = db_entry->chains_buf;

    const int chains_cnt = db_entry->chains_cnt;

    qsort (chains_buf, chains_cnt, sizeof (chain_t), sort_by_ks);
  }

  /**
   * sort global order by password length counts
   */

  for (int pw_len = pw_min, order_pos = 0; pw_len <= pw_max; pw_len++, order_pos++)
  {
    db_entry_t *db_entry = &db_entries[pw_len];

    const u64 elems_cnt = db_entry->elems_cnt;

    pw_order_t *pw_order = &pw_orders[order_pos];

    pw_order->len = pw_len;
    pw_order->cnt = elems_cnt;
  }

  const int order_cnt = pw_max + 1 - pw_min;

  qsort (pw_orders, order_cnt, sizeof (pw_order_t), sort_by_cnt);

  /**
   * seek to some starting point
   */

  if (mpz_cmp_si (skip, 0))
  {
    if (mpz_cmp (skip, total_ks_cnt) >= 0)
    {
      fprintf (stderr, "Value of --skip must be smaller than total keyspace\n");

      return (-1);
    }
  }

  if (mpz_cmp_si (limit, 0))
  {
    if (mpz_cmp (limit, total_ks_cnt) > 0)
    {
      fprintf (stderr, "Value of --limit cannot be larger than total keyspace\n");

      return (-1);
    }

    mpz_add (tmp, skip, limit);

    if (mpz_cmp (tmp, total_ks_cnt) > 0)
    {
      fprintf (stderr, "Value of --skip + --limit cannot be larger than total keyspace\n");

      return (-1);
    }

    mpz_set (total_ks_cnt, tmp);
  }

  /**
   * skip to the first main loop that will output a password
   */

  if (mpz_cmp_si (skip, 0))
  {
    mpz_t skip_left;  mpz_init_set (skip_left, skip);
    mpz_t main_loops; mpz_init (main_loops);

    u64 outs_per_main_loop = 0;

    for (int pw_len = pw_min; pw_len <= pw_max; pw_len++)
    {
      mpz_init_set_si (pw_ks_pos[pw_len], 0);

      outs_per_main_loop += wordlen_dist[pw_len];
    }

    // find pw_ks_pos[]

    while (1)
    {
      mpz_fdiv_q_ui (main_loops, skip_left, outs_per_main_loop);

      if (mpz_cmp_si (main_loops, 0) == 0)
      {
        break;
      }

      // increment the main loop "main_loops" times

      for (int pw_len = pw_min; pw_len <= pw_max; pw_len++)
      {
        if (mpz_cmp (pw_ks_pos[pw_len], pw_ks_cnt[pw_len]) < 0)
        {
          mpz_mul_ui (tmp, main_loops, wordlen_dist[pw_len]);

          mpz_add (pw_ks_pos[pw_len], pw_ks_pos[pw_len], tmp);

          mpz_sub (skip_left, skip_left, tmp);

          if (mpz_cmp (pw_ks_pos[pw_len], pw_ks_cnt[pw_len]) > 0)
          {
            mpz_sub (tmp, pw_ks_pos[pw_len], pw_ks_cnt[pw_len]);

            mpz_add (skip_left, skip_left, tmp);
          }
        }
      }

      outs_per_main_loop = 0;

      for (int pw_len = pw_min; pw_len <= pw_max; pw_len++)
      {
        if (mpz_cmp (pw_ks_pos[pw_len], pw_ks_cnt[pw_len]) < 0)
        {
          outs_per_main_loop += wordlen_dist[pw_len];
        }
      }
    }

    mpz_sub (total_ks_pos, skip, skip_left);

    // set db_entries to pw_ks_pos[]

    for (int pw_len = pw_min; pw_len <= pw_max; pw_len++)
    {
      db_entry_t *db_entry = &db_entries[pw_len];

      int      chains_cnt = db_entry->chains_cnt;
      chain_t *chains_buf = db_entry->chains_buf;

      mpz_set (tmp, pw_ks_pos[pw_len]);

      for (int chains_idx = 0; chains_idx < chains_cnt; chains_idx++)
      {
        chain_t *chain_buf = &chains_buf[chains_idx];

        if (mpz_cmp (tmp, chain_buf->ks_cnt) < 0)
        {
          mpz_set (chain_buf->ks_pos, tmp);

          set_chain_ks_poses (chain_buf, db_entries, tmp, db_entry->cur_chain_ks_poses);

          break;
        }

        mpz_sub (tmp, tmp, chain_buf->ks_cnt);

        db_entry->chains_pos++;
      }
    }

    // clean up

    for (int pw_len = pw_min; pw_len <= pw_max; pw_len++)
    {
      mpz_clear (pw_ks_cnt[pw_len]);
      mpz_clear (pw_ks_pos[pw_len]);
    }

    mpz_clear (skip_left);
    mpz_clear (main_loops);
  }

  /**
   * loop
   */

  while (mpz_cmp (total_ks_pos, total_ks_cnt) < 0)
  {
    for (int order_pos = 0; order_pos < order_cnt; order_pos++)
    {
      pw_order_t *pw_order = &pw_orders[order_pos];

      const int pw_len = pw_order->len;

      char pw_buf[BUFSIZ];

      pw_buf[pw_len] = '\n';

      db_entry_t *db_entry = &db_entries[pw_len];

      const u64 outs_cnt = wordlen_dist[pw_len];

      u64 outs_pos = 0;

      while (outs_pos < outs_cnt)
      {
        const int chains_cnt = db_entry->chains_cnt;
        const int chains_pos = db_entry->chains_pos;

        if (chains_pos == chains_cnt) break;

        chain_t *chains_buf = db_entry->chains_buf;

        chain_t *chain_buf = &chains_buf[chains_pos];

        mpz_sub (total_ks_left, total_ks_cnt, total_ks_pos);

        mpz_sub (iter_max, chain_buf->ks_cnt, chain_buf->ks_pos);

        if (mpz_cmp (total_ks_left, iter_max) < 0)
        {
          mpz_set (iter_max, total_ks_left);
        }

        const u64 outs_left = outs_cnt - outs_pos;

        mpz_set_ui (tmp, outs_left);

        if (mpz_cmp (tmp, iter_max) < 0)
        {
          mpz_set (iter_max, tmp);
        }

        const u64 iter_max_u64 = mpz_get_ui (iter_max);

        mpz_add (tmp, total_ks_pos, iter_max);

        if (mpz_cmp (tmp, skip) > 0)
        {
          u64 iter_pos_u64 = 0;

          if (mpz_cmp (total_ks_pos, skip) < 0)
          {
            mpz_sub (tmp, skip, total_ks_pos);

            iter_pos_u64 = mpz_get_ui (tmp);

            mpz_add (tmp, chain_buf->ks_pos, tmp);

            set_chain_ks_poses (chain_buf, db_entries, tmp, db_entry->cur_chain_ks_poses);
          }

          u64 *cur_chain_ks_poses = db_entry->cur_chain_ks_poses;

          chain_set_pwbuf_init (chain_buf, db_entries, cur_chain_ks_poses, pw_buf);

          while (iter_pos_u64 < iter_max_u64)
          {
            out_push (out, pw_buf, pw_len + 1);

            chain_set_pwbuf_increment (chain_buf, db_entries, cur_chain_ks_poses, pw_buf);

            iter_pos_u64++;
          }
        }
        else
        {
          mpz_add (tmp, chain_buf->ks_pos, iter_max);

          set_chain_ks_poses (chain_buf, db_entries, tmp, db_entry->cur_chain_ks_poses);
        }

        outs_pos += iter_max_u64;

        mpz_add (total_ks_pos, total_ks_pos, iter_max);

        mpz_add (chain_buf->ks_pos, chain_buf->ks_pos, iter_max);

        if (mpz_cmp (chain_buf->ks_pos, chain_buf->ks_cnt) == 0)
        {
          db_entry->chains_pos++;

          // db_entry->cur_chain_ks_poses[] should of cycled to all zeros, but just in case?

          memset (db_entry->cur_chain_ks_poses, 0, ELEM_CNT_MAX * sizeof (u64));
        }

        if (mpz_cmp (total_ks_pos, total_ks_cnt) == 0) break;
      }

      if (mpz_cmp (total_ks_pos, total_ks_cnt) == 0) break;
    }
  }

  out_flush (out);

  /**
   * cleanup
   */

  mpz_clear (iter_max);
  mpz_clear (total_ks_cnt);
  mpz_clear (total_ks_pos);
  mpz_clear (total_ks_left);
  mpz_clear (skip);
  mpz_clear (limit);
  mpz_clear (tmp);

  for (int pw_len = pw_min; pw_len <= pw_max; pw_len++)
  {
    db_entry_t *db_entry = &db_entries[pw_len];

    if (db_entry->chains_buf)
    {
      int      chains_cnt = db_entry->chains_cnt;
      chain_t *chains_buf = db_entry->chains_buf;

      for (int chains_idx = 0; chains_idx < chains_cnt; chains_idx++)
      {
        chain_t *chain_buf = &chains_buf[chains_idx];

        mpz_clear (chain_buf->ks_cnt);
        mpz_clear (chain_buf->ks_pos);
      }

      free (db_entry->chains_buf);
    }

    if (db_entry->elems_buf)  free (db_entry->elems_buf);
  }

  free (out);
  free (wordlen_dist);
  free (pw_orders);
  free (db_entries);

  return 0;
}

