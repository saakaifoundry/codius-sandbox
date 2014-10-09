#include <unistd.h>
#include <stdlib.h>
#include <memory.h>
#include <errno.h>
#include <error.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ptrace.h>
#include <sys/user.h>

#include <sys/select.h>

#include <sched.h>

#include "codius-util.h"

#define ORIG_EAX 11

#define PTRACE_EVENT_SECCOMP 7

int main(int argc, char** argv)
{
  pid_t child;
  int api_fds[2];

  if (socketpair (AF_UNIX, SOCK_STREAM, 0, api_fds) == -1 ){
    error (EXIT_FAILURE, errno, "Could not create IPC channel");
  }

  child = fork();
  if (child) {
    int status;

    close (api_fds[1]);

    ptrace (PTRACE_ATTACH, child, 0, 0);
    waitpid (child, &status, 0);
    ptrace (PTRACE_SETOPTIONS, child, 0,
        PTRACE_O_EXITKILL | PTRACE_O_TRACESECCOMP );
    ptrace (PTRACE_CONT, child, 0, 0);

    while (!WIFEXITED (status)) {
      sigset_t mask;
      fd_set read_set;
      struct timeval timeout;
      memset (&timeout, 0, sizeof (timeout));
      memset (&read_set, 0, sizeof (read_set));
      memset (&mask, 0, sizeof (mask));

      FD_ZERO (&read_set);
      FD_SET (api_fds[0], &read_set);

      int ready_count = select (1, &read_set, 0, 0, &timeout);

      waitpid (child, &status, WNOHANG);

      if (ready_count > 0) {
        char buf[1024];
        printf ("Got an RPC message!\n");
        read (api_fds[0], buf, sizeof (buf));
        buf[sizeof (buf)] = 0;
        printf ("Buf: %s", buf);
      } else if (WSTOPSIG (status) == SIGSEGV) {
        char *use_debugger = getenv ("CODIUS_SANDBOX_USE_DEBUGGER");
        if (use_debugger && strcmp(use_debugger, "1") == 0) {
          char pidstr[15];
          sprintf (pidstr, "%d", child);
          printf ("Got segv, launching debugger on %s\n", pidstr);
          ptrace (PTRACE_DETACH, child, 0, SIGSTOP);
          _exit (execlp ("gdb", "gdb", "-p", pidstr, NULL));
        }
      } else if (WSTOPSIG (status) == SIGTRAP && (status >> 8) == (SIGTRAP | PTRACE_EVENT_SECCOMP << 8)) {
        struct user_regs_struct regs;
        memset (&regs, 0, sizeof (regs));
        if (ptrace (PTRACE_GETREGS, child, 0, &regs) < 0) {
          error (EXIT_FAILURE, errno, "Failed to fetch registers");
        }
        printf ("Sandboxed module tried syscall %ld\n", regs.orig_rax);
      }
      ptrace (PTRACE_CONT, child, 0, 0);
    }
    printf ("Sandboxed module exited: %d\n", WEXITSTATUS (status));
    return WEXITSTATUS (status);
  } else {
    close (api_fds[0]);
    dup2 (api_fds[1], 3);

    printf ("Launching %s\n", argv[1]);
    ptrace (PTRACE_TRACEME, 0, 0);
    raise (SIGSTOP);
    setenv ("LD_PRELOAD", "./out/Default/lib.target/libcodius-sandbox.so", 1);
    if (execvp (argv[1], &argv[1]) < 0) {
      error(EXIT_FAILURE, errno, "Could not start sandboxed module:");
    }
  }
}
