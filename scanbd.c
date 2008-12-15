/*
 * $Header: /cvsroot/kmux/kmux-generate/helper/sane/scanbd/scanbd.c,v 1.8 2008/12/15 10:26:59 wimalopaan Exp $
 *
 *  scanbd - KMUX scanner button daemon
 *
 *  Copyright (C) 2008  Wilhelm Meier (wilhelm.meier@fh-kl.de)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "scanbd.h"

cfg_t* cfg = NULL;

// the actual values of the command-line options
struct scanbdOptions scanbd_options = {
    /* manager-mode */     false,
    /* foreground */       false,
    /* signal */	   false,
    /* config_file_name */ "scanbd.conf"
};

// the options for getopt_long()
static struct option options[] = {
    {"manager", 0, NULL, 'm'},
    {"signal", 0, NULL, 's'},
    {"debug", 0, NULL, 'd'},
    {"foreground", 0, NULL, 'f'},
    {"config", 1, NULL, 'c'},
    {"trigger", 1, NULL, 't'},
    {"action", 1, NULL, 'a'},
    { 0, 0, NULL, 0}
};

// parsing the config-file via libconfuse
void cfg_do_parse(void) {
    slog(SLOG_INFO, "reading config file %s", scanbd_options.config_file_name);

    cfg_opt_t cfg_numtrigger[] = {
	CFG_INT(C_FROM_VALUE, C_FROM_VALUE_DEF_INT, CFGF_NONE),
	CFG_INT(C_TO_VALUE, C_TO_VALUE_DEF_INT, CFGF_NONE),
	CFG_END()
    };

    cfg_opt_t cfg_strtrigger[] = {
	CFG_STR(C_FROM_VALUE, C_FROM_VALUE_DEF_STR, CFGF_NONE),
	CFG_STR(C_TO_VALUE, C_TO_VALUE_DEF_STR, CFGF_NONE),
	CFG_END()
    };
    
    cfg_opt_t cfg_action[] = {
	CFG_STR(C_FILTER, C_ACTION_DEF, CFGF_NONE),
	CFG_SEC(C_NUMERICAL_TRIGGER, cfg_numtrigger, CFGF_NONE),
	CFG_SEC(C_STRING_TRIGGER, cfg_strtrigger, CFGF_NONE),
	CFG_STR(C_DESC, C_DESC_DEF, CFGF_NONE),
	CFG_STR(C_SCRIPT, C_SCRIPT_DEF, CFGF_NONE),
	CFG_END()
    };

    cfg_opt_t cfg_function[] = {
	CFG_STR(C_FILTER, C_FUNCTION_DEF, CFGF_NONE),
	CFG_STR(C_DESC, C_DESC_DEF, CFGF_NONE),
	CFG_STR(C_ENV, C_ENV_FUNCTION_DEF, CFGF_NONE),
	CFG_END()
    };

    cfg_opt_t cfg_environment[] = {
	CFG_STR(C_ENV_DEVICE, C_ENV_DEVICE_DEF, CFGF_NONE),
	CFG_STR(C_ENV_ACTION, C_ENV_ACTION_DEF, CFGF_NONE),
	CFG_END()
    };

    cfg_opt_t cfg_global[] = {
	CFG_BOOL(C_DEBUG, C_DEBUG_DEF, CFGF_NONE),
	CFG_INT(C_DEBUG_LEVEL, C_DEBUG_LEVEL_DEF, CFGF_NONE),
	CFG_STR(C_USER, C_USER_DEF, CFGF_NONE),
	CFG_STR(C_GROUP, C_GROUP_DEF, CFGF_NONE),
	CFG_STR(C_SANED, C_SANED_DEF, CFGF_NONE),
	CFG_STR_LIST(C_SANED_OPTS, C_SANED_OPTS_DEF, CFGF_NONE),
	CFG_INT(C_TIMEOUT, C_TIMEOUT_DEF, CFGF_NONE),
	CFG_STR(C_PIDFILE, C_PIDFILE_DEF, CFGF_NONE),
	CFG_SEC(C_ENVIRONMENT, cfg_environment, CFGF_NONE),
	CFG_SEC(C_FUNCTION, cfg_function, CFGF_MULTI | CFGF_TITLE),
	CFG_SEC(C_ACTION, cfg_action, CFGF_MULTI | CFGF_TITLE),
	CFG_END()
    };

    cfg_opt_t cfg_device[] = {
	CFG_STR(C_FILTER, "^fujitsu.*", CFGF_NONE),
	CFG_STR(C_DESC, C_DESC_DEF, CFGF_NONE),
	CFG_SEC(C_ACTION, cfg_action, CFGF_MULTI | CFGF_TITLE),
	CFG_END()
    };

    cfg_opt_t cfg_options[] = {
	CFG_SEC(C_GLOBAL, cfg_global, CFGF_NONE),
	CFG_SEC(C_DEVICE, cfg_device, CFGF_MULTI | CFGF_TITLE),
	CFG_END()
    };

    if (cfg) {
	cfg_free(cfg);
	cfg = NULL;
    }
    
    cfg = cfg_init(cfg_options, CFGF_NONE);

    int ret = 0;
    if ((ret = cfg_parse(cfg, scanbd_options.config_file_name)) != CFG_SUCCESS) {
	if (CFG_FILE_ERROR == ret) {
	    slog(SLOG_ERROR, "can't open config file: %s", scanbd_options.config_file_name);
	    exit(EXIT_FAILURE);
	}
	else {
	    slog(SLOG_ERROR, "parse error in config file");
	    exit(EXIT_FAILURE);
	}
	exit(EXIT_FAILURE); // not reached
    }

    cfg_t* cfg_sec_global = NULL;
    assert((cfg_sec_global = cfg_getsec(cfg, C_GLOBAL)) != NULL);
    debug |= cfg_getbool(cfg_sec_global, C_DEBUG);
    debug_level = cfg_getint(cfg_sec_global, C_DEBUG_LEVEL);
}

void sig_hup_handler(int signal) {
    slog(SLOG_DEBUG, "sig_hup_handler called");
    (void)signal;
    // stop all threads
    stop_sane_threads();
    cfg_do_parse();
    start_sane_threads();
    // start all threads
}

void sig_usr1_handler(int signal) {
    slog(SLOG_DEBUG, "sig_usr1_handler called");
    (void)signal;
    // stop all threads
    stop_sane_threads();
}

void sig_usr2_handler(int signal) {
    slog(SLOG_DEBUG, "sig_usr2_handler called");
    (void)signal;
    // start all threads
    start_sane_threads();
}

void sig_term_handler(int signal) {
    slog(SLOG_DEBUG, "sig_term/int_handler called with signal %d", signal);
    // stop all threads
    stop_sane_threads();

    // get the name of the pidfile
    const char* pidfile = NULL;
    cfg_t* cfg_sec_global = NULL;
    assert((cfg_sec_global = cfg_getsec(cfg, "global")) != NULL);
    assert((pidfile = cfg_getstr(cfg_sec_global, "pidfile")) != NULL);

    // reclaim the old uid (root) to unlink the pidfile
    // mostly neccessary if the pidfile lives in /var/run
    if (setuid((pid_t)0) < 0) {
	slog(SLOG_WARN, "Can't acquire uid root to unlink pidfile %s : %s",
	     pidfile, strerror(errno));
	// not an hard error, since sometimes this isn't neccessary
    }
    else {	
	if (unlink(pidfile) < 0) {
	    slog(SLOG_ERROR, "Can't unlink pidfile: %s", strerror(errno));
	    exit(EXIT_FAILURE);
	}
    }
    exit(EXIT_SUCCESS);
}

int main(int argc, char** argv) {
    // init the logging feature
    slog_init(argv[0]);

    // install all the signalhandlers early
    // SIGHUP rereads the config as usual
    struct sigaction sa;
    memset(&sa, 0, sizeof(struct sigaction));
    sa.sa_handler = sig_hup_handler;
    sigemptyset(&sa.sa_mask);    
    // prevent interleaving with stop/start_sane_threads 
    sigaddset(&sa.sa_mask, SIGUSR1);
    sigaddset(&sa.sa_mask, SIGUSR2);
    if (sigaction(SIGHUP, &sa, NULL) < 0) {
	slog(SLOG_ERROR, "Can't install signalhandler for SIGHUP: %s", strerror(errno));
	exit(EXIT_FAILURE);
    }

    // SIGUSR1 is used to stop all polling threads
    memset(&sa, 0, sizeof(struct sigaction));
    sa.sa_handler = sig_usr1_handler;
    sigemptyset(&sa.sa_mask);
    // prevent interleaving the stop_sane_threads with start_sane_threads 
    sigaddset(&sa.sa_mask, SIGUSR2);
    sigaddset(&sa.sa_mask, SIGHUP);
    if (sigaction(SIGUSR1, &sa, NULL) < 0) {
	slog(SLOG_ERROR, "Can't install signalhandler for SIGUSR1: %s", strerror(errno));
	exit(EXIT_FAILURE);
    }

    // SIGUSR2 is used to restart all polling threads
    memset(&sa, 0, sizeof(struct sigaction));
    sa.sa_handler = sig_usr2_handler;
    sigemptyset(&sa.sa_mask);    
    // prevent interleaving the start_sane_threads with stop_sane_threads 
    sigaddset(&sa.sa_mask, SIGUSR1);
    sigaddset(&sa.sa_mask, SIGHUP);
    if (sigaction(SIGUSR2, &sa, NULL) < 0) {
	slog(SLOG_ERROR, "Can't install signalhandler for SIGUSR2: %s", strerror(errno));
	exit(EXIT_FAILURE);
    }

    // SIGTERM and SIGINT terminates the process gracefully
    memset(&sa, 0, sizeof(struct sigaction));
    sa.sa_handler = sig_term_handler;
    sigemptyset(&sa.sa_mask);    
    if (sigaction(SIGTERM, &sa, NULL) < 0) {
	slog(SLOG_ERROR, "Can't install signalhandler for SIGTERM: %s", strerror(errno));
	exit(EXIT_FAILURE);
    }
    if (sigaction(SIGINT, &sa, NULL) < 0) {
	slog(SLOG_ERROR, "Can't install signalhandler for SIGINT: %s", strerror(errno));
	exit(EXIT_FAILURE);
    }

    int trigger_device = -1;
    int trigger_action = -1;

    // read the options of the commandline
    while(true) {
	int option_index = 0;
	int c = 0;
	if ((c = getopt_long(argc, argv, "mc:dft:a:", options, &option_index)) < 0) {
	    break;
	}
	switch(c) {
	case 'm':
	    slog(SLOG_INFO, "manager-mode");
	    scanbd_options.managerMode = true;
	    break;
	case 's':
	    slog(SLOG_INFO, "signal-mode");
	    scanbd_options.signal = true;
	    break;
	case 'd':
	    slog(SLOG_INFO, "debug on");
	    debug = true;
	    break;
	case 'f':
	    slog(SLOG_INFO, "forground");
	    scanbd_options.foreground = true;
	    break;
	case 'c':
	    slog(SLOG_INFO, "config-file: %s", optarg);
	    scanbd_options.config_file_name = strdup(optarg);
	    break;
	case 't':
	    slog(SLOG_INFO, "trigger for device number: %d", atoi(optarg));
	    trigger_device = atoi(optarg);
	    scanbd_options.foreground = true;
	    break;
	case 'a':
	    slog(SLOG_INFO, "trigger action number: %d", atoi(optarg));
	    trigger_action = atoi(optarg);
	    scanbd_options.foreground = true;
	    break;
	default:
	    break;
	}
    }

    cfg_do_parse();

    if (debug) {
	slog(SLOG_INFO, "debug on: level: %d", debug_level);
    }
    else {
	slog(SLOG_INFO, "debug off");
    }

    // manager-mode in signal-mode
    // stops all polling threads in a running scanbd by sending
    // SIGUSR1
    // then starts saned
    // afterwards restarts all polling threads in a running scanbd by
    // sending SIGUSR2

    // manager-mode in dbus-mode
    // stops all polling threads in a running scanbd by calling
    // a dbus message of the running scanbd
    // then starts saned
    // afterwards restarts all polling threads in a running scanbd by
    // another dbus message

    // this is usefull for using scanbd in manager-mode from inetd
    // starting saned indirectly
    // in inetd.conf:
    //
    if (scanbd_options.managerMode) {
	slog(SLOG_DEBUG, "Entering manager mode");

	if ((trigger_device >= 0) && (trigger_action >= 0)) {
	    slog(SLOG_DEBUG, "Entering trigger mode");
	    dbus_call_trigger(trigger_device, trigger_action);
	    exit(EXIT_SUCCESS);
	}
	else {
	    slog(SLOG_DEBUG, "device and action must be >= 0");
	    exit(EXIT_FAILURE);
	}
	
	pid_t scanbd_pid = -1;
	// get the name of the saned executable
	const char* saned = NULL;
	cfg_t* cfg_sec_global = NULL;
	assert((cfg_sec_global = cfg_getsec(cfg, "global")) != NULL);
	assert((saned = cfg_getstr(cfg_sec_global, "saned")) != NULL);

	if (scanbd_options.signal) {
	    slog(SLOG_DEBUG, "manager mode: signal");
	    // get the path of the pid-file of the running scanbd
	    const char* scanbd_pid_file = NULL;
	    assert((scanbd_pid_file = cfg_getstr(cfg_sec_global, "pidfile")) != NULL);

	    // get the pid of the running scanbd out of the pidfile
	    FILE* pidfile;
	    if ((pidfile = fopen(scanbd_pid_file, "r")) == NULL) {
		slog(SLOG_WARN, "Can't open pidfile %s", scanbd_pid_file);
	    }
	    else {
		char pida[NAME_MAX];
		if (fgets(pida, NAME_MAX, pidfile) == NULL) {
		    slog(SLOG_WARN, "Can't read pid from pidfile %s", scanbd_pid_file);
		}
		if (fclose(pidfile) < 0) {
		    slog(SLOG_WARN, "Can't close pidfile %s", scanbd_pid_file);
		}
		scanbd_pid = atoi(pida);
		slog(SLOG_DEBUG, "found scanbd with pid %d", scanbd_pid);
		// set scanbd to sleep mode
		slog(SLOG_DEBUG, "sending SIGUSR1", scanbd_pid);
		if (kill(scanbd_pid, SIGUSR1) < 0) {
		    slog(SLOG_WARN, "Can't send signal SIGUSR1 to pid %d: %s",
			 scanbd_pid, strerror(errno));
		    slog(SLOG_DEBUG, "uid=%d, euid=%d", getuid(), geteuid());
		    //exit(EXIT_FAILURE);
		}
	    }
	    // sleep some time to give the other scanbd to close all the
	    // usb-connections 
	    sleep(1);
	} // signal-mode
	else {
	    slog(SLOG_DEBUG, "manager mode: dbus");
	    slog(SLOG_DEBUG, "calling dbus method: %s", SCANBD_DBUS_METHOD_ACQUIRE);
	    dbus_call_method(SCANBD_DBUS_METHOD_ACQUIRE, NULL);
	}
	// start the real saned
	slog(SLOG_DEBUG, "forking subprocess for saned");
	pid_t spid = 0;
	if ((spid = fork()) < 0) {
	    slog(SLOG_ERROR, "fork for saned subprocess failed: %s", strerror(errno));
	    exit(EXIT_FAILURE);
	}
	else if (spid > 0) { // parent
	    // wait for the saned process to finish
	    int status = 0;
	    slog(SLOG_DEBUG, "waiting for saned");
	    if (waitpid(spid, &status, 0) < 0) {
		slog(SLOG_ERROR, "waiting for saned failed: %s", strerror(errno));
		exit(EXIT_FAILURE);
	    }
	    if (WIFEXITED(status)) {
		slog(SLOG_INFO, "saned exited with status: %d", WEXITSTATUS(status));
	    }
	    // saned finished and now
	    // reactivate scandb
	    if (scanbd_options.signal) {
		// sleep some time to give the other scanbd to close all the
		// usb-connections 
		sleep(1);
		if (scanbd_pid > 0) {
		    slog(SLOG_DEBUG, "sending SIGUSR2");
		    if (kill(scanbd_pid, SIGUSR2) < 0) {
			slog(SLOG_INFO, "Can't send signal SIGUSR1 to pid %d: %s",
			     scanbd_pid, strerror(errno));
		    }
		}
	    } // signal-mode
	    else {
		slog(SLOG_DEBUG, "calling dbus method: %s", SCANBD_DBUS_METHOD_RELEASE);
		dbus_call_method(SCANBD_DBUS_METHOD_RELEASE, NULL);
	    }
	}
	else { // child
	    if (execl(saned, "saned", NULL) < 0) {
		slog(SLOG_ERROR, "exec of saned failed: %s", strerror(errno));
		exit(EXIT_FAILURE);
	    }
	    exit(EXIT_SUCCESS); // not reached
	}
	exit(EXIT_SUCCESS);
    }
    else { // not in manager mode

	// daemonize,
	if (!scanbd_options.foreground) {
	    slog(SLOG_DEBUG, "daemonize");
	    daemonize();
	}

	cfg_t* cfg_sec_global = NULL;
	assert((cfg_sec_global = cfg_getsec(cfg, C_GLOBAL)) != NULL);

	// drop the privilegies
	const char* euser = NULL;
	assert((euser = cfg_getstr(cfg_sec_global, C_USER)) != NULL);
	const char* egroup = NULL;
	assert((egroup = cfg_getstr(cfg_sec_global, C_GROUP)) != NULL);

	struct passwd* pwd = NULL;
	if ((pwd = getpwnam(euser)) == NULL) {
	    slog(SLOG_ERROR, "No user %s: %s", euser, strerror(errno));
	    exit(EXIT_FAILURE);
	}
	assert(pwd);

	struct group* grp = NULL;
	if ((grp = getgrnam(egroup)) == NULL) {
	    slog(SLOG_ERROR, "No group %s: %s", egroup, strerror(errno));
	    exit(EXIT_FAILURE);
	}
	assert(grp);
	
	// write pid file
	const char* pidfile = NULL;
	assert((pidfile = cfg_getstr(cfg_sec_global, "pidfile")) != NULL);

	int pid_fd = 0;
	if ((pid_fd = open(pidfile, O_RDWR | O_CREAT | O_EXCL,
			   S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)) < 0) {
	    slog(SLOG_ERROR, "Can't create pidfile %s : %s", pidfile, strerror(errno));
	    exit(EXIT_FAILURE);
	}
	else {
	    if (ftruncate(pid_fd, 0) < 0) {
		slog(SLOG_ERROR, "Can't clear pidfile: %s", strerror(errno));
		exit(EXIT_FAILURE);
	    }
	    char buf[80];
	    snprintf(buf, 79, "%d\n", getpid());
	    if (write(pid_fd, buf, strlen(buf)) < 0) {
		slog(SLOG_ERROR, "Can't write pidfile: %s", strerror(errno));
		exit(EXIT_FAILURE);
	    }
	    if (close(pid_fd)) {
		slog(SLOG_ERROR, "Can't close pidfile: %s", strerror(errno));
		exit(EXIT_FAILURE);
	    }
	    if (chown(pidfile, pwd->pw_uid, grp->gr_gid) < 0) {
		slog(SLOG_ERROR, "Can't chown pidfile: %s", strerror(errno));
		exit(EXIT_FAILURE);
	    }
	}

	// drop the privileges
	// first change our effective gid 
	if (grp != NULL) {
	    slog(SLOG_DEBUG, "drop privileges to gid: %d", grp->gr_gid);
	    if (setegid(grp->gr_gid) < 0) {
		slog(SLOG_WARN, "Can't set the effective gid to %d", grp->gr_gid);
	    }
	    else {
		slog(SLOG_INFO, "Running as effective gid %d", grp->gr_gid);
	    }
	}
	// then change our effective uid
	if (pwd != NULL) {
	    slog(SLOG_DEBUG, "drop privileges to uid: %d", pwd->pw_uid);
	    if (seteuid(pwd->pw_uid) < 0) {
		slog(SLOG_WARN, "Can't set the effective uid to %d", pwd->pw_uid);
	    }
	    else {
		slog(SLOG_INFO, "Running as effective uid %d", pwd->pw_uid);
	    }
	}

	// Init DBus well known interface
	// must be possible with the user from config file
	dbus_init();

	// Init SANE
	SANE_Int sane_version = 0;
	sane_init(&sane_version, 0);
	slog(SLOG_INFO, "sane version %d.%d",
	     SANE_VERSION_MAJOR(sane_version),
	     SANE_VERSION_MINOR(sane_version));

	// get all devices locally connected to the system 
	get_sane_devices();

	// start the polling threads
	start_sane_threads();

	// start dbus thread
	dbus_start_dbus_thread();
	
	// well, sit here and wait ...
	// this thread executes the signal handlers
	while(true) {
	    if (pause() < 0) {
		slog(SLOG_DEBUG, "pause: %s", strerror(errno));
	    }
	}
    }
    exit(EXIT_SUCCESS); // never reached
}
