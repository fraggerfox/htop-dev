/*
htop - TraceScreen.c
(C) 2005-2006 Hisham H. Muhammad
Released under the GNU GPLv2+, see the COPYING file
in the source distribution for its full text.
*/

#include "config.h" // IWYU pragma: keep

#include "TraceScreen.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/wait.h>

#include "CRT.h"
#include "FunctionBar.h"
#include "Panel.h"
#include "ProvideCurses.h"
#include "XUtils.h"


static const char* const TraceScreenFunctions[] = {"Search ", "Filter ", "AutoScroll ", "Stop Tracing   ", "Done   ", NULL};

static const char* const TraceScreenKeys[] = {"F3", "F4", "F8", "F9", "Esc"};

static const int TraceScreenEvents[] = {KEY_F(3), KEY_F(4), KEY_F(8), KEY_F(9), 27};

TraceScreen* TraceScreen_new(const Process* process) {
   // This initializes all TraceScreen variables to "false" so only default = true ones need to be set below
   TraceScreen* this = xCalloc(1, sizeof(TraceScreen));
   Object_setClass(this, Class(TraceScreen));
   this->tracing = true;
   this->strace_alive = false;
   FunctionBar* fuBar = FunctionBar_new(TraceScreenFunctions, TraceScreenKeys, TraceScreenEvents);
   CRT_disableDelay();
   return (TraceScreen*) InfoScreen_init(&this->super, process, fuBar, LINES - 2, " ");
}

void TraceScreen_delete(Object* cast) {
   TraceScreen* this = (TraceScreen*) cast;
   if (this->child > 0) {
      kill(this->child, SIGTERM);
      while (waitpid(this->child, NULL, 0) == -1)
         if (errno != EINTR)
            break;
   }

   if (this->strace) {
      fclose(this->strace);
   }

   CRT_enableDelay();
   free(InfoScreen_done((InfoScreen*)this));
}

static void TraceScreen_draw(InfoScreen* this) {
   InfoScreen_drawTitled(this, "Trace of process %d - %s", Process_getPid(this->process), Process_getCommand(this->process));
}

bool TraceScreen_forkTracer(TraceScreen* this) {
   int fdpair[2] = {0, 0};

   if (pipe(fdpair) == -1)
      return false;

   if (fcntl(fdpair[0], F_SETFL, O_NONBLOCK) < 0)
      goto err;

   if (fcntl(fdpair[1], F_SETFL, O_NONBLOCK) < 0)
      goto err;

   pid_t child = fork();
   if (child == -1)
      goto err;

   if (child == 0) {
      close(fdpair[0]);

      dup2(fdpair[1], STDOUT_FILENO);
      dup2(fdpair[1], STDERR_FILENO);
      close(fdpair[1]);

      char buffer[32] = {0};
      xSnprintf(buffer, sizeof(buffer), "%d", Process_getPid(this->super.process));

      #if defined(HTOP_FREEBSD) || defined(HTOP_OPENBSD) || defined(HTOP_NETBSD) || defined(HTOP_DRAGONFLYBSD) || defined(HTOP_SOLARIS)
         // Use of NULL in variadic functions must have a pointer cast.
         // The NULL constant is not required by standard to have a pointer type.
         execlp("truss", "truss", "-s", "512", "-p", buffer, (void*)NULL);

         // Should never reach here, unless execlp fails ...
         const char* message = "Could not execute 'truss'. Please make sure it is available in your $PATH.";
         (void)! write(STDERR_FILENO, message, strlen(message));
      #elif defined(HTOP_LINUX)
         execlp("strace", "strace", "-T", "-tt", "-s", "512", "-p", buffer, (void*)NULL);

         // Should never reach here, unless execlp fails ...
         const char* message = "Could not execute 'strace'. Please make sure it is available in your $PATH.";
         (void)! write(STDERR_FILENO, message, strlen(message));
      #else // HTOP_DARWIN, HTOP_PCP == HTOP_UNSUPPORTED
         const char* message = "Tracing unavailable on not supported system.";
         (void)! write(STDERR_FILENO, message, strlen(message));
      #endif

      exit(127);
   }

   FILE* fp = fdopen(fdpair[0], "r");
   if (!fp)
      goto err;

   close(fdpair[1]);

   this->child = child;
   this->strace = fp;
   this->strace_alive = true;

   return true;

err:
   close(fdpair[1]);
   close(fdpair[0]);
   return false;
}

static void TraceScreen_updateTrace(InfoScreen* super) {
   TraceScreen* this = (TraceScreen*) super;

   int fd_strace = fileno(this->strace);

   fd_set fds;
   FD_ZERO(&fds);
   FD_SET(STDIN_FILENO, &fds);
   if (this->strace_alive) {
      assert(fd_strace != -1);
      FD_SET(fd_strace, &fds);
   }

   struct timeval tv = { .tv_sec = 0, .tv_usec = 500 };
   int ready = select(MAXIMUM(STDIN_FILENO, fd_strace) + 1, &fds, NULL, NULL, &tv);

   char buffer[1025];
   size_t nread = 0;
   if (ready > 0 && FD_ISSET(fd_strace, &fds))
      nread = fread(buffer, 1, sizeof(buffer) - 1, this->strace);

   if (nread && this->tracing) {
      const char* line = buffer;
      buffer[nread] = '\0';
      for (size_t i = 0; i < nread; i++) {
         if (buffer[i] == '\n') {
            buffer[i] = '\0';
            if (this->contLine) {
               InfoScreen_appendLine(&this->super, line);
               this->contLine = false;
            } else {
               InfoScreen_addLine(&this->super, line);
            }
            line = buffer + i + 1;
         }
      }
      if (line < buffer + nread) {
         InfoScreen_addLine(&this->super, line);
         buffer[nread] = '\0';
         this->contLine = true;
      }
      if (this->follow) {
         Panel_setSelected(this->super.display, Panel_size(this->super.display) - 1);
      }
   } else {
      if (this->strace_alive && waitpid(this->child, NULL, WNOHANG) != 0)
         this->strace_alive = false;
   }
}

static bool TraceScreen_onKey(InfoScreen* super, int ch) {
   TraceScreen* this = (TraceScreen*) super;

   switch (ch) {
      case 'f':
      case KEY_F(8):
         this->follow = !(this->follow);
         if (this->follow)
            Panel_setSelected(super->display, Panel_size(super->display) - 1);
         return true;
      case 't':
      case KEY_F(9):
         this->tracing = !this->tracing;
         FunctionBar_setLabel(super->display->defaultBar, KEY_F(9), this->tracing ? "Stop Tracing   " : "Resume Tracing ");
         InfoScreen_draw(this);
         return true;
   }

   this->follow = false;
   return false;
}

const InfoScreenClass TraceScreen_class = {
   .super = {
      .extends = Class(Object),
      .delete = TraceScreen_delete
   },
   .draw = TraceScreen_draw,
   .onErr = TraceScreen_updateTrace,
   .onKey = TraceScreen_onKey,
};
