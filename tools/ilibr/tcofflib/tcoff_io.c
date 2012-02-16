/*
 *	TCOFF I/O functions
 *	Copyright (C) 1990 Inmos Limited
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation; either version 2 of the License, or
 *	(at your option) any later version.
 *
 *	This program is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *	GNU General Public License for more details.
 *
 *	You should have received a copy of the GNU General Public License
 *	along with this program; if not, write to the Free Software
 *	Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/* Copyright 1990 INMOS Limited */
/* CMSIDENTIFIER
   PLAY:TCOFF_IO_C@523.AAAA-FILE;1(20-FEB-92)[FROZEN] */

/* Ade 22/3/93 - Added const to tcoff_putrec, bug INSdi01122 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <stdio.h>
#ifdef __STDC__
#include <stdlib.h>
#include <stdarg.h>
#else
#include <varargs.h>
#endif
#include <string.h>
#include "toolkit.h"

PRIVATE long int bytes_to_tag[] =
{
  0L,       /* not used */
  251L,     /* 1 byte  use 1 byte   */
  252L,     /* 2 bytes use 2 bytes  */
  253L,     /* 3 bytes use 4 bytes  */
  253L,     /* 4 bytes use ...      */
  254L,     /* 5 bytes use 8 bytes  */
  254L,     /* 6 bytes use ...      */
  254L,     /* 7 bytes use ...      */
  254L      /* 8 bytes use ...      */
};

PRIVATE void write_error (int n)
{
  fprintf (stderr, "Fatal-<inmos library>-low level write failed %d\n", n);
  exit (EXIT_FAILURE);
}
/*{{{   PUBLIC long int tcoff_getl_test (fs, ok)   */
PUBLIC long int tcoff_getl_test (fs, ok)
FILE *fs;
int *ok;
{
  long int res;
  int size, i, c, neg;
  if ((c = fgetc (fs)) == EOF)
  {
    *ok = FALSE;
    return (0L);
  }
  else *ok = TRUE;
  if (c == 255)
  {
    neg = TRUE;
    if ((c = fgetc (fs)) == EOF)
    {
      *ok = FALSE;
      return (0L);
    }
  }
  else if (c == 254)
  {
    *ok = FALSE;
    return (0L);
  }
  else neg = FALSE;
  if ((0 <= c) && (c <= 250)) res = (long int) c;
  else
  {
    size = 1 << (c - 251);
    res = 0L;
    for (i = 0; i < size; i++)
    {
      c = fgetc (fs);
      if (c == EOF)
      {
        *ok = FALSE;
        return (0L);
      }
      res = res | (((long int) c) << (8 * i)) ;
    }
  }
  if (neg) return (~res);
  else return (res);
}
/*}}}*/
/*{{{   PUBLIC unsigned long int tcoff_getul_test (fs, ok)   */
PUBLIC unsigned long int tcoff_getul_test (fs, ok)
FILE *fs;
int *ok;
{
  int i, c;
  unsigned long int l;
  *ok = TRUE;
  l = 0;
  for (i = 0; i < 4; i++)
  {
    c = fgetc (fs);
    if (c == EOF)
    {
      *ok = FALSE;
      return (0L);
    }
    l = l | (((unsigned long int) c) << (8 * i)) ;
  }
  return (l);
}
/*}}}*/
/*{{{   PUBLIC char *tcoff_gets_test (fs, len, ok)   */
PUBLIC char *tcoff_gets_test (fs, len, ok)
FILE *fs;
long int *len;
int *ok;
{
  int l, i, c;
  char *str;
  *len = tcoff_getl_test (fs, ok);
  if (!*ok) return (NULL);
  l = (int) *len;
  str = malloc_chk (1 + l * sizeof (char));
  for (i = 0; i < l; i++)
  {
    c = fgetc (fs);
    if (c == EOF)
    {
      *ok = FALSE;
      return (NULL);
    }
    str[i] = c;
  }
  str[i] = '\0';
  return (str);
}
/*}}}*/

/*{{{   PUBLIC void tcoff_putl (fs, l)   */
PUBLIC void tcoff_putl (fs, l)
FILE *fs;
long int l;
{
  int size, i;
  long int n, bytes;
  if (l < 0L)
  {
    if (fputc (255, fs) == EOF) write_error (1);
    l = ~l;
  }
  if ((0L <= l) && (l <= 250L)) fputc ((int) l, fs);
  else
  {
    size = 1;
    n = l >> 8;
    while (n != 0L)
    {
      n = n >> 8;
      size++;
    }
    if (fputc ((int) bytes_to_tag[size], fs) == EOF) write_error (2);
    bytes = 1L << (bytes_to_tag[size] - 251);
    for (i = 0; i < bytes; i++)
    {
      if (fputc ((int) (l & 0xFFL), fs) == EOF) write_error (3);
      l = l >> 8;
    }
  }
}
/*}}}*/
/*{{{   PUBLIC void tcoff_putul (fs, l)   */
PUBLIC void tcoff_putul (fs, l)
FILE *fs;
unsigned long int l;
{
  int i;
  for (i = 0; i < 4; i++)
  {
    if (fputc ((int) (l & 0xFFL), fs) == EOF) write_error (4);
    l = l >> 8;
  }
}
/*}}}*/
/*{{{   PUBLIC void tcoff_puts (fs, size, string)   */
PUBLIC void tcoff_puts (fs, size, string)
FILE *fs;
long int size;
char *string;
{
  tcoff_putl (fs, size);
  if (fwrite (string, (size_t) sizeof (char), (size_t) size, fs) < size) write_error (5);
}
/*}}}*/
/*{{{   PUBLIC long int tcoff_sizel (l)   */
PUBLIC long int tcoff_sizel (l)
long int l;
{
  long int size, inc;
  if (l < 0L)
  {
    size = 1L;   /* leading 255 specifies negative number */
    l = ~l;
  }
  else size = 0L;
  if ((0L <= l) && (l <= 250L)) size++;  /* number only */
  else
  {
    size++;          /* store number of following bytes (powers of 2) */
    inc = 1L;        /* we know we need at least one */
    l = l >> 8;
    while (l != 0L)
    {
      l = l >> 8;
      inc++;
    }
    size += 1L << (bytes_to_tag[inc] - 251);
  }
  return (size);
}
/*}}}*/
/*{{{   PRIVATE long int _tcoff_record_length (ap, va_alist)   */
PRIVATE long int _tcoff_record_length (ap, va_alist)
va_list ap;
#ifdef __STDC__
char *va_alist;
#else
va_dcl
#endif
{
  long int res, l;
  unsigned long int dummy;
  char *p, *fmt;
  res = 0L;
#ifdef __STDC__
  fmt = va_alist;
#else
  fmt = va_arg (ap, char *);
#endif
  for (p = fmt; *p; p++)
  {
    /*{{{   if (*p == '%') switch (*++p)   */
    if (*p == '%') switch (*++p)
    {
      case 's':
        l = (long int) strlen (va_arg (ap, char *));
        res += (tcoff_sizel (l) + l);
        break;
      case 'l':
        switch (*++p)
          {
            case 'd':
              res += tcoff_sizel (va_arg (ap, long int));
              break;
            case 'u':
              res += 4L;
              dummy = va_arg (ap, unsigned long int);
              break;
            default:
              fprintf (stderr, "characters 'l%c' unknown in record string. please report", *p);
              exit (EXIT_FAILURE);
              break;
          }
        break;
      default:
        fprintf (stderr, "character %c unknown in record string. please report", *p);
        exit (EXIT_FAILURE);
        break;
    }
    /*}}}*/
    else if (*p != ' ')
    {
      fprintf (stderr, "character %c unknown in record string. please report", *p);
      exit (EXIT_FAILURE);
    }
  }
  return (res);
}
/*}}}*/
/*{{{   PRIVATE void _tcoff_print_rec (fs, ap, va_alist)   */
PRIVATE void _tcoff_print_rec (fs, ap, va_alist)
FILE *fs;
va_list ap;
#ifdef __STDC__
char *va_alist;
#else
va_dcl
#endif
{
  long int l;
  char *p, *str, *fmt;
#ifdef __STDC__
  fmt = va_alist;
#else
  fmt = va_arg (ap, char *);
#endif
  for (p = fmt; *p; p++)
  {
    /*{{{   if (*p == '%') switch (*++p)   */
    if (*p == '%') switch (*++p)
    {
      case 's':
        str = va_arg (ap, char *);
        l = (long int) strlen (str);
        tcoff_puts (fs, l, str);
        break;
      case 'l': switch (*++p)
                {
                  case 'd': tcoff_putl (fs, va_arg (ap, long int)); break;
                  case 'u': tcoff_putul (fs, va_arg (ap, unsigned long int)); break;
                  default:  fprintf (stderr, "characters 'l%c' unknown in record string. please report", *p);
                            exit (EXIT_FAILURE);
                            break;
                }
                break;
      default:
        fprintf (stderr, "character %c unknown in record string. please report", *p);
        exit (EXIT_FAILURE);
        break;
    }
    /*}}}*/
    else if (*p != ' ')
    {
      fprintf (stderr, "character %c unknown in record string. please report", *p);
      exit (EXIT_FAILURE);
    }
  }
}
/*}}}*/
/*{{{   PUBLIC void tcoff_putrec (FILE *fs, long int tag, char *va_alist, ...)   */
#ifdef __STDC__
PUBLIC void tcoff_putrec (FILE *fs, long int tag, const char *va_alist, ...)
#else
PUBLIC void tcoff_putrec (fs, tag, va_alist)
FILE *fs;
long int tag;
va_dcl
#endif
{
  va_list ap;
  long int rec_len;

  tcoff_putl (fs, tag);
#ifdef __STDC__
  va_start (ap, va_alist);
#else
  va_start (ap);
#endif
  rec_len = _tcoff_record_length (ap, va_alist);
  va_end (ap);

  tcoff_putl (fs, rec_len);
#ifdef __STDC__
  va_start (ap, va_alist);
#else
  va_start (ap);
#endif
  _tcoff_print_rec (fs, ap, va_alist);
  va_end (ap);
}
/*}}}*/
