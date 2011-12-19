/*
 * Copyright (C) 2011 Bumblebee Project
 * Author: Joaquín Ignacio Aramendía <samsagax@gmail.com>
 * Author: Jaron Viëtor AKA "Thulinma" <jaron@vietors.com>
 *
 * This file is part of Bumblebee.
 *
 * Bumblebee is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Bumblebee is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Bumblebee. If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * C-coded version of the Bumblebee daemon and optirun.
 */

#include "config.h"
#include "bbconfig.h"
#include "bbsocket.h"
#include "bblogger.h"
#include "bbrun.h"
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <grp.h>
#include <signal.h>
#include <time.h>

/**
 *  Print a little note on usage
 */
static void print_usage(int exit_val) {
  // Print help message and exit with exit code
  printf("%s version %s\n\n", bb_status.program_name, GITVERSION);
  printf("Usage: %s [options] -- [application to run] [application options]\n", bb_status.program_name);
  printf("  Options:\n");
  printf("      -q\tBe quiet.\n");
  printf("      -v\tBe verbose (twice for extra verbosity)\n");
  printf("      -X #\tX display number to use.\n");
  printf("      -l [PATH]\tLD driver path to use.\n");
  printf("      -u [PATH]\tUnix socket to use.\n");
  printf("      -m [METHOD]\tConnection method to use for VirtualGL.\n");
  printf("      -h\tShow this help screen.\n");
  printf("\n");
  printf("If no application is given, current status is instead shown.\n");
  printf("\n");
  exit(exit_val);
}

/**
 *  Handle recieved signals - except SIGCHLD, which is handled in bbrun.c
 */
static void handle_signal(int sig) {
  switch (sig) {
    case SIGHUP:
      bb_log(LOG_WARNING, "Received %s signal (ignoring...)\n", strsignal(sig));
      break;
    case SIGINT:
    case SIGQUIT:
    case SIGTERM:
      bb_log(LOG_WARNING, "Received %s signal.\n", strsignal(sig));
      socketClose(&bb_status.bb_socket); //closing the socket terminates the server
      break;
    default:
      bb_log(LOG_WARNING, "Unhandled signal %s\n", strsignal(sig));
      break;
  }
}

int main(int argc, char* argv[]) {

  /* Setup signal handling before anything else */
  signal(SIGHUP, handle_signal);
  signal(SIGTERM, handle_signal);
  signal(SIGINT, handle_signal);
  signal(SIGQUIT, handle_signal);

  /* Initializing configuration */
  bb_status.program_name = argv[0];
  bb_status.is_daemonized = 0;
  bb_status.verbosity = VERB_WARN;
  bb_status.errors[0] = 0; //no errors, yet :-)
  bb_status.runmode = BB_RUN_APP;
  read_configuration();

  /* Parse the options, set flags as necessary */
  int c;
  while ((c = getopt(argc, argv, "+cvVm:X:l:u:h|help")) != -1) {
    switch (c) {
      case 'h'://help
        print_usage(EXIT_SUCCESS);
        break;
      case 'q'://quiet mode
        bb_status.verbosity = VERB_NONE;
        break;
      case 'v'://verbose
        // -v -v is very verbose
        if (bb_status.verbosity == VERB_INFO) {
          bb_status.verbosity = VERB_DEBUG;
        } else {
          bb_status.verbosity = VERB_INFO;
        }
        break;
      case 'X'://X display number
        snprintf(bb_config.x_display, BUFFER_SIZE, "%s", optarg);
        break;
      case 'l'://LD driver path
        snprintf(bb_config.ld_path, BUFFER_SIZE, "%s", optarg);
        break;
      case 'u'://Unix socket to use
        snprintf(bb_config.socket_path, BUFFER_SIZE, "%s", optarg);
        break;
      case 'm'://vglclient method
        snprintf(bb_config.vgl_compress, BUFFER_SIZE, "%s", optarg);
        break;
      default:
        // Unrecognized option
        print_usage(EXIT_FAILURE);
        break;
    }
  }

  /* change runmode to status if no application given to run
   * and current runmode is run application.
   */
  if ((bb_status.runmode == BB_RUN_APP) && (optind >= argc)) {
    bb_status.runmode = BB_RUN_STATUS;
  }

  /* Init log Mechanism */
  if (bb_init_log()) {
    fprintf(stderr, "Unexpected error, could not initialize log.\n");
    return 1;
  }
  bb_log(LOG_DEBUG, "%s version %s starting...\n", bb_status.program_name, GITVERSION);

  /* Connect to listening daemon */
  bb_status.bb_socket = socketConnect(bb_config.socket_path, SOCK_NOBLOCK);
  if (bb_status.bb_socket < 0) {
    bb_log(LOG_ERR, "Could not connect to bumblebee daemon - is it running?\n");
    bb_closelog();
    return EXIT_FAILURE;
  }
  char buffer[BUFFER_SIZE];
  int r;

  /* Request status */
  if (bb_status.runmode == BB_RUN_STATUS) {
    r = snprintf(buffer, BUFFER_SIZE, "Status?");
    socketWrite(&bb_status.bb_socket, buffer, r);
    while (bb_status.bb_socket != -1) {
      r = socketRead(&bb_status.bb_socket, buffer, BUFFER_SIZE);
      if (r > 0) {
        printf("Bumblebee status: %*s\n", r, buffer);
        socketClose(&bb_status.bb_socket);
      }
    }
  }

  /* Run given application */
  if (bb_status.runmode == BB_RUN_APP) {
    r = snprintf(buffer, BUFFER_SIZE, "Checking availability...");
    socketWrite(&bb_status.bb_socket, buffer, r);
    while (bb_status.bb_socket != -1) {
      r = socketRead(&bb_status.bb_socket, buffer, BUFFER_SIZE);
      if (r > 0) {
        bb_log(LOG_INFO, "Response: %*s\n", r, buffer);
        switch (buffer[0]) {
          case 'N': //No, run normally.
            socketClose(&bb_status.bb_socket);
            bb_log(LOG_WARNING, "Running application normally.\n");
            bb_run_exec(argv + optind);
            break;
          case 'Y': //Yes, run through vglrun
            bb_log(LOG_INFO, "Running application through vglrun.\n");
            //run vglclient if any method other than proxy is used
            if (strncmp(bb_config.vgl_compress, "proxy", BUFFER_SIZE) != 0) {
              char * vglclient_args[] = {
                "vglclient",
                "-detach",
                0
              };
              bb_run_fork(vglclient_args);
            }
            char ** vglrun_args = malloc(sizeof (char *) * (9 + argc - optind));
            vglrun_args[0] = "vglrun";
            vglrun_args[1] = "-c";
            vglrun_args[2] = bb_config.vgl_compress;
            vglrun_args[3] = "-d";
            vglrun_args[4] = bb_config.x_display;
            vglrun_args[5] = "-ld";
            vglrun_args[6] = bb_config.ld_path;
            vglrun_args[7] = "--";
            for (r = 0; r < argc - optind; r++) {
              vglrun_args[8 + r] = argv[optind + r];
            }
            vglrun_args[8 + r] = 0;
            bb_run_fork_wait(vglrun_args);
            socketClose(&bb_status.bb_socket);
            break;
          default: //Something went wrong - output and exit.
            bb_log(LOG_ERR, "Problem: %*s\n", r, buffer);
            socketClose(&bb_status.bb_socket);
            break;
        }
      }
    }
  }

  bb_closelog();
  bb_stop_all(); //stop any started processes that are left
  return (EXIT_SUCCESS);
}