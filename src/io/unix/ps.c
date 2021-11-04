/*!
 * ps.c - process functions for libsatoshi
 * Copyright (c) 2020, Christopher Jeffrey (MIT License).
 * https://github.com/chjj/libsatoshi
 */

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <io/core.h>

/*
 * Globals
 */

static void (*global_handler)(void *) = NULL;
static void *global_arg = NULL;

/*
 * Process
 */

int
btc_ps_cwd(char *buf, size_t size) {
  if (size < 2)
    return 0;

#if defined(__wasi__)
  buf[0] = '/';
  buf[1] = '\0';
#else
  if (getcwd(buf, size) == NULL)
    return 0;

  buf[size - 1] = '\0';
#endif

  return 1;
}

int
btc_ps_getenv(char *out, size_t size, const char *name) {
  char *value = getenv(name);
  size_t len;

  if (value == NULL)
    return 0;

  len = strlen(value);

  if (len + 1 > size)
    return 0;

  memcpy(out, value, len + 1);

  return 1;
}

int
btc_ps_daemon(void) {
#if defined(__wasi__) || defined(__EMSCRIPTEN__)
  return 0;
#else
  pid_t pid = fork();

  if (pid < 0)
    return 0;

  if (pid > 0) {
    exit(EXIT_SUCCESS);
    return 1;
  }

  if (setsid() < 0) {
    exit(EXIT_FAILURE);
    return 0;
  }

  close(STDIN_FILENO);
  close(STDOUT_FILENO);
  close(STDERR_FILENO);

  return 1;
#endif
}

static void
btc_signal(int signal, void (*handler)(int)) {
  struct sigaction sa;

  sa.sa_handler = handler;

  sigemptyset(&sa.sa_mask);

  sa.sa_flags = 0;

  sigaction(signal, &sa, NULL);
}

static void
real_handler(int signum) {
  (void)signum;

  if (global_handler != NULL) {
    global_handler(global_arg);
    global_handler = NULL;
  }
}

void
btc_ps_onterm(void (*handler)(void *), void *arg) {
  global_handler = handler;
  global_arg = arg;

  btc_signal(SIGTERM, real_handler);
  btc_signal(SIGINT, real_handler);
}
