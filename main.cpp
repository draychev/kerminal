#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main() {
  const char *argv[] = {"konsole", nullptr};
  execvp("konsole", const_cast<char *const *>(argv));
  fprintf(stderr, "kerminal: failed to launch konsole: %s\n", strerror(errno));
  return 1;
}
