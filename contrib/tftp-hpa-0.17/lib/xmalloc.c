/*
 * xmalloc.c
 *
 * Simple error-checking version of malloc()
 *
 */

#include <stdlib.h>
#include <stdio.h>

void *xmalloc(size_t size)
{
  void *p = malloc(size);

  if ( !p ) {
    fprintf(stderr, "Out of memory!\n");
    exit(128);
  }

  return p;
}
