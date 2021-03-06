/*
 * $Id$
 *
 *  scanbd - KMUX scanner button daemon
 *
 *  Copyright (C) 2008 - 2017 Wilhelm Meier (wilhelm.wm.meier@googlemail.com)
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
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "scanbd.h"
#include "scanbd_dbus.h"

#define CANCEL_TEST

// all programm-global sane functions use this mutex to avoid races
#ifdef PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
// this is non-portable
static pthread_mutex_t sane_mutex = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
#else
static pthread_mutex_t sane_mutex;
#endif

static pthread_cond_t  sane_cv    = PTHREAD_COND_INITIALIZER;

// the following locking strategie must be obeyed:
// 1) lock the sane_mutex
// 2) lock the device specific mutex
// in this order to avoid deadlocks
// holding more than these two locks is not intended

#ifndef PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
void sane_init_mutex()
{
    slog(SLOG_INFO, "sane_init_mutex");
    pthread_mutexattr_t mutexattr;
    if (pthread_mutexattr_init(&mutexattr) < 0) {
        slog(SLOG_ERROR, "Can't initialize mutex attr");
        exit(EXIT_FAILURE);
    }
    if (pthread_mutexattr_settype(&mutexattr, PTHREAD_MUTEX_RECURSIVE) < 0) {
        slog(SLOG_ERROR, "Can't set mutex attr");
        exit(EXIT_FAILURE);
    }
    if (pthread_mutex_init(&sane_mutex, &mutexattr) < 0) {
        slog(SLOG_ERROR, "Can't init mutex");
        exit(EXIT_FAILURE);
    }
}
#endif

struct sane_opt_value {    
    unsigned long num_value; // before-value or after-value or actual-value (BOOL|INT|FIXED)
    struct {                 // (STRING)
        char*     str;       // actual-value
        regex_t*  reg;       // before-regex or after-regex
    } str_value;
};
typedef struct sane_opt_value sane_opt_value_t;

struct sane_dev_option {
    int number;                  // the option-number of the device-option
    sane_opt_value_t from_value; // the before-value of the option
    sane_opt_value_t to_value;   // the after-value of the option (to
    // fire the trigger)
    sane_opt_value_t value;      // the option value (from the last
    // polling cycle)
    const char* script;          // the found (matched) script to be called if
    // the option-valued changes
    const char* action_name;	 // the name of this action as
    // specified in the config file
};
typedef struct sane_dev_option sane_dev_option_t;

struct sane_dev_function {
    int number;			 // the option-number of the
    // device-option
    const char* env;		 // the name of the environment-var to
    // pass to option value in
};
typedef struct sane_dev_function sane_dev_function_t;

// each polling thread is represented by struct sane_thread
// there is no locking, since this is "thread private data"
struct sane_thread {
    pthread_t tid;                   // the thread-id of the polling
    // thread
    pthread_mutex_t mutex;	     // mutex for this data-structure
    pthread_cond_t cv;		     // cv for this data-structure
    bool triggered;		     // a rule for this device has fired (triggered == true)
    int  triggered_option;           // the action number which triggered
    const SANE_Device* dev;          // the device
    int num_of_options;	             // the number of all options for
    // this device
    SANE_Handle h;                   // the handle of the opened device
    sane_dev_option_t *opts;         // the list of matched actions
    // for this device
    int num_of_options_with_scripts; // the number of elements in the
    // above list
    sane_dev_function_t *functions;  // the list of matched functions
    // for this device
    int num_of_options_with_functions;// the number of elements in the
    // above list
};
typedef struct sane_thread sane_thread_t;

// the list of all polling threads
static sane_thread_t* sane_poll_threads = NULL;

// the list of all devices locally connected to our system
static const SANE_Device** sane_device_list = NULL;

// the number of devices = the number of polling threads
static int num_devices = 0;

void get_sane_devices(void) {
    // detect all the scanners we have
    slog(SLOG_INFO, "Scanning for local-only devices" );

    if (pthread_mutex_lock(&sane_mutex) < 0) {
        // if we can't get the mutex, something is heavily wrong!
        slog(SLOG_ERROR, "pthread_mutex_lock: %s", strerror(errno));
        return;
    }
    SANE_Status sane_status = SANE_STATUS_INVAL;
    sane_device_list = NULL;
    num_devices = 0;
    if ((sane_status = sane_get_devices(&sane_device_list, SANE_TRUE)) != SANE_STATUS_GOOD) {
        slog(SLOG_WARN, "Can't get the sane device list");
    }
    const SANE_Device** dev = sane_device_list;
    if (dev == NULL) {
        slog(SLOG_DEBUG, "device list null");
        goto cleanup;
    }
    while(*dev != NULL) {
        slog(SLOG_DEBUG, "found device: %s %s %s %s",
             (*dev)->name, (*dev)->vendor, (*dev)->model, (*dev)->type);
        num_devices += 1;
        dev++;
    }
    if (pthread_cond_broadcast(&sane_cv)) {
        slog(SLOG_ERROR, "pthread_cond_broadcast: %s", strerror(errno));
    }
cleanup:
    if (pthread_mutex_unlock(&sane_mutex) < 0) {
        // if we can't unlock the mutex, something is heavily wrong!
        slog(SLOG_ERROR, "pthread_mutex_unlock: %s", strerror(errno));
        return;
    }
}	

// simple hash function for C-strings
static unsigned long hash(const char *str) {
    unsigned long hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
    }
    return hash;
}

static void sane_option_value_init(sane_opt_value_t* v) {
    v->num_value = 0;
    v->str_value.str = NULL;
    v->str_value.reg = NULL;
}

static void sane_option_value_free(sane_opt_value_t* v) {
    if (v->str_value.str != NULL) {
        free((void*)v->str_value.str);
        v->str_value.str = NULL;
    }
    if (v->str_value.reg != NULL) {
        regfree(v->str_value.reg);
        free(v->str_value.reg);
        v->str_value.reg = NULL;
    }
}

static sane_opt_value_t get_sane_option_value(SANE_Handle* h, int index) {
    slog(SLOG_DEBUG, "get_sane_option_value");
    // get the value of option with index of the device (opened) with
    // handle h
    // if option can't be found or other catastrophy happens, the
    // value 0 gets returned
#if ((__STDC_VERSION__  - 0) < 201112L) || ((__GNUC__ - 0) < 5)
    sane_opt_value_t res;
#else
    sane_opt_value_t res = {};
#endif

    sane_option_value_init(&res);

    const SANE_Option_Descriptor* odesc = NULL;
    if ((odesc = sane_get_option_descriptor(h, index)) == NULL) {
        return res;
    }
    if ((odesc->type == SANE_TYPE_BOOL) || (odesc->type == SANE_TYPE_INT) ||
            (odesc->type == SANE_TYPE_FIXED) || (odesc->type == SANE_TYPE_BUTTON)) {
        unsigned long int value = 0;
        if ((unsigned int)odesc->size <= sizeof(long int)) {
            //if we can store it in an long int
            SANE_Status status = SANE_STATUS_INVAL;
            if ((status = sane_control_option(h, index, SANE_ACTION_GET_VALUE,
                                              &value, NULL)) != SANE_STATUS_GOOD) {
                slog(SLOG_WARN, "Can't read value of %s: %s",
                     odesc->name, sane_strstatus(status));
                return res;
            }
            res.num_value = value;
            return res;
        }
        else {
            // shouldn't happen
            slog(SLOG_WARN, "Value of %s, sane-type %d too big", odesc->name, odesc->type);
            return res;
        }
    }
    else if (odesc->type == SANE_TYPE_STRING) {
        res.str_value.str = calloc(odesc->size + 1, sizeof(char));
        assert(res.str_value.str != NULL);
        SANE_Status status = SANE_STATUS_INVAL;
        if ((status = sane_control_option(h, index, SANE_ACTION_GET_VALUE,
                                          res.str_value.str, NULL)) != SANE_STATUS_GOOD) {
            slog(SLOG_WARN, "Can't read value of %s: %s", odesc->name, sane_strstatus(status));
            return res;
        }
        res.str_value.str[odesc->size] = '\0';
        size_t slen = strlen(res.str_value.str);
        res.num_value = hash(res.str_value.str);

        slog(SLOG_INFO, "Value of %s as string (len %d, hash %d): %s",
             odesc->name, slen, res.num_value, res.str_value.str);
        return res;
    }
    else {
        slog(SLOG_WARN, "Can't read option %s of type %d", odesc->name, odesc->type);
    }
    return res;
}


// cleanup handler for sane_poll
static void sane_thread_cleanup_mutex(void* arg) {
    assert(arg != NULL);
    slog(SLOG_DEBUG, "sane_thread_cleanup_mutex");
    pthread_mutex_t* mutex = (pthread_mutex_t*)arg;
    if (pthread_mutex_unlock(mutex) < 0) {
        // if we can't unlock the mutex, something is heavily wrong!
        slog(SLOG_ERROR, "pthread_mutex_unlock: %s", strerror(errno));
    }
}

// cleanup handler for sane_poll
static void sane_thread_cleanup_value(void* arg) {
    assert(arg != NULL);
    slog(SLOG_DEBUG, "sane_thread_cleanup_value");
    sane_opt_value_t* v = (sane_opt_value_t*)arg;
    sane_option_value_free(v);
}


// this function can only be used in the critical region of *st
static void sane_find_matching_functions(sane_thread_t* st, cfg_t* sec) {
    // TODO: use of recursive mutex???
    slog(SLOG_DEBUG, "sane_find_matching_functions");
    const char* title = cfg_title(sec);
    if (title == NULL) {
        title = SCANBD_NULL_STRING;
    }
    int functions = cfg_size(sec, C_FUNCTION);
    if (functions <= 0) {
        slog(SLOG_INFO, "no matching functions in section %s", title);
        return;
    }
    
    slog(SLOG_INFO, "found %d functions in section %s", functions, title);
    // iterate over all global functions
    for(int i = 0; i < functions; i += 1) {
        // get the function from the config file
        cfg_t* function_i = cfg_getnsec(sec, C_FUNCTION, i);
        assert(function_i != NULL);

        // get the filter-regex from the config-file
        const char* opt_regex = cfg_getstr(function_i, C_FILTER);
        assert(opt_regex != NULL);

        const char* title = cfg_title(function_i);
        if (title == NULL) {
            title = "(none)";
        }
        // compile the filter-regex
        slog(SLOG_DEBUG, "checking function %s with filter: %s",
             title, opt_regex);
        regex_t creg;
        int ret = regcomp(&creg, opt_regex, REG_EXTENDED | REG_NOSUB);
        if (ret < 0) {
            char err_text[1024];
            regerror(ret, &creg, err_text, 1024);
            slog(SLOG_WARN, "Can't compile regex: %s : %s", opt_regex, err_text);
            continue;
        }
        // look for matching option-names
        for(int opt = 1; opt < st->num_of_options; opt += 1) {
            const SANE_Option_Descriptor* odesc = NULL;
            if ((odesc = sane_get_option_descriptor(st->h, opt)) == NULL) {
                // no valid option-descriptor available
                // skip it
                slog(SLOG_INFO, "option[%d] has no valid descriptor", opt);
                continue;
            }
            assert(odesc);
            if (!SANE_OPTION_IS_ACTIVE(odesc->cap)) {
                slog(SLOG_INFO, "option[%d] is not active", opt);
                continue;
            }
            // option is active
            // only use active (user controllable) options
            if (odesc->name == NULL) {
                // we need a valid option-name
                slog(SLOG_INFO, "option[%d] has no name", opt);
                continue;
            }
            assert(odesc->name);
            if (!((odesc->type == SANE_TYPE_BOOL) || (odesc->type == SANE_TYPE_INT) ||
                  (odesc->type == SANE_TYPE_FIXED)|| (odesc->type == SANE_TYPE_STRING) ||
                  (odesc->type == SANE_TYPE_BUTTON))) {
                slog(SLOG_WARN, "option[%d] %s for device %s not of "
                     "type BOOL|INT|FIXED|STRING|BUTTON. Skipping",
                     opt, odesc->name, st->dev->name);
                continue;
            }
            slog(SLOG_INFO, "found active option[%d] %s (type: %d) for device %s",
                 opt, odesc->name, odesc->type, st->dev->name);
            // regex compare with the filter
            if (regexec(&creg, odesc->name, 0, NULL, 0) != 0) {
                // no match
                continue;
            }
            // match

            // now get the script
            const char* env = cfg_getstr(function_i, C_ENV);
            assert(env != NULL);
            slog(SLOG_INFO, "installing function %s for %s, option[%d]: %s as env: %s",
                 title, st->dev->name, opt, odesc->name, env);

            // looking for option already present in the
            // array
            int n = 0;
            for(n = 0; n < st->num_of_options_with_functions; n += 1) {
                if (st->functions[n].number == opt) {
                    slog(SLOG_WARN, "function %s overrides function of option[%d]",
                         title, n);
                    // break out with n == index_of_found_option
                    break;
                }
            }
            // 0 <= n < st->num_of_options_with_scripts:
            // we found it
            // n == st->num_of_options_with_scripts:
            // not found => new

            st->functions[n].number = opt;
            st->functions[n].env = env;

            if (n == st->num_of_options_with_functions) {
                // not found in the list
                // we have a new option to be polled
                st->num_of_options_with_functions += 1;
            }
        } // foreach option
        // this compiled regex isn't used anymore
        regfree(&creg);
    } // foreach action
}

// this function can only be used in the critical region of *st
static void sane_find_matching_options(sane_thread_t* st, cfg_t* sec) {
    slog(SLOG_DEBUG, "sane_find_matching_options");
    const char* title = cfg_title(sec);
    if (title == NULL) {
        title = SCANBD_NULL_STRING;
    }
    // TODO: use of recursive mutex???
    int actions = cfg_size(sec, C_ACTION);
    if (actions <= 0) {
        slog(SLOG_INFO, "no matching actions in section %s",  title);
        return;
    }
    
    slog(SLOG_INFO, "found %d actions in section %s", actions, title);

    // iterate over all global actions
    for(int i = 0; i < actions; i += 1) {
        // get the action from the config file
        cfg_t* action_i = cfg_getnsec(sec, C_ACTION, i);
        assert(action_i != NULL);

        // get the filter-regex from the config-file
        const char* opt_regex = cfg_getstr(action_i, C_FILTER);
        assert(opt_regex != NULL);

        const char* title = cfg_title(action_i);
        if (title == NULL) {
            title = "(none)";
        }
        // compile the filter-regex
        slog(SLOG_DEBUG, "checking action %s with filter: %s",
             title, opt_regex);
        regex_t creg;
        int ret = regcomp(&creg, opt_regex, REG_EXTENDED | REG_NOSUB);
        if (ret < 0) {
            char err_text[1024];
            regerror(ret, &creg, err_text, 1024);
            slog(SLOG_WARN, "Can't compile regex: %s : %s", opt_regex, err_text);
            continue;
        }
        // look for matching option-names
        for(int opt = 1; opt < st->num_of_options; opt += 1) {
            const SANE_Option_Descriptor* odesc = NULL;
            if ((odesc = sane_get_option_descriptor(st->h, opt)) == NULL) {
                // no valid option-descriptor available
                // skip it
                continue;
            }
            assert(odesc);
            if (!SANE_OPTION_IS_ACTIVE(odesc->cap)) {
                continue;
            }
            // option is active
            // only use active (user controllable) options
            if (odesc->name == NULL) {
                // we need a valid option-name
                continue;
            }
            assert(odesc->name);
            if (!((odesc->type == SANE_TYPE_BOOL) || (odesc->type == SANE_TYPE_INT) ||
                  (odesc->type == SANE_TYPE_FIXED)|| (odesc->type == SANE_TYPE_STRING) ||
                  (odesc->type == SANE_TYPE_BUTTON))) {
                slog(SLOG_WARN, "option[%d] %s for device %s not of "
                     "type BOOL|INT|FIXED|STRING|BUTTON. Skipping",
                     opt, odesc->name, st->dev->name);
                continue;
            }
            slog(SLOG_INFO, "found active option[%d] %s (type: %d) for device %s",
                 opt, odesc->name, odesc->type, st->dev->name);
            // regex compare with the filter
            if (regexec(&creg, odesc->name, 0, NULL, 0) != 0) {
                // no match
                continue;
            }
            // match

            // now get the script from the action

            const char* script = cfg_getstr(action_i, C_SCRIPT);

            if (!script || (strlen(script) == 0)) {
                script = SCANBD_NULL_STRING;
            }       

            assert(script != NULL);
            slog(SLOG_INFO, "installing action %s (%d) for %s, option[%d]: %s as: %s",
                 title, st->num_of_options_with_scripts, st->dev->name, opt, odesc->name, script);

            // get pointer to global section of config

            cfg_t* cfg_sec_global = NULL;
            cfg_sec_global = cfg_getsec(cfg, C_GLOBAL);
            assert(cfg_sec_global);

            bool multiple_actions = cfg_getbool(cfg_sec_global, C_MULTIPLE_ACTIONS);
            slog(SLOG_INFO, "multiple actions allowed");

            // looking for option already present in the
            // array
            int n = 0;
            for(n = 0; n < st->num_of_options_with_scripts; n += 1) {
                if (st->opts[n].number == opt) {
                    if (!multiple_actions) {
                        slog(SLOG_WARN, "action %s overrides script %s of option[%d] with %s",
                             title, st->opts[n].script, opt, script);
                        // break out with n == index_of_found_option
                        break;
                    }
                    else {
                        if (n < st->num_of_options) {
                            n = st->num_of_options_with_scripts;
                            slog(SLOG_INFO, "adding additional action %s (%d) for option[%d] with %s",
                                 title, n, opt, script);
                            break;
                        }
                        else {
                            slog(SLOG_INFO, "can't add additional action %s for option[%d] with %s",
                                 title, opt, script);
                            break;
                        }
                    }
                }
            }
            // 0 <= n < st->num_of_options_with_scripts:
            // we found it (override now)
            // n == st->num_of_options_with_scripts:
            // not found => new

            if (n == st->num_of_options) {
                continue; // no space left in array
            }
            st->opts[n].number = opt;
            st->opts[n].action_name = title;
            st->opts[n].script = script;
            sane_option_value_free(&st->opts[n].from_value);
            sane_option_value_free(&st->opts[n].to_value);
            sane_option_value_free(&st->opts[n].value);

            if ((odesc->type == SANE_TYPE_BOOL) || (odesc->type == SANE_TYPE_INT) ||
                    (odesc->type == SANE_TYPE_FIXED) || (odesc->type == SANE_TYPE_BUTTON)) {
                // numerical option
                cfg_t* num_trigger = cfg_getsec(action_i, C_NUMERICAL_TRIGGER);
                assert(num_trigger);
                st->opts[n].from_value.num_value = cfg_getint(num_trigger,
                                                              C_FROM_VALUE);
                st->opts[n].to_value.num_value = cfg_getint(num_trigger, C_TO_VALUE);

                st->opts[n].value = get_sane_option_value(st->h, opt);

                slog(SLOG_INFO, "Initial value of option %s is %d", odesc->name,
                     st->opts[n].value);
            } // type BOOL | INT || FIXED
            else if (odesc->type == SANE_TYPE_STRING) {
                bool valid = true;
                // string option
                cfg_t* str_trigger = cfg_getsec(action_i, C_STRING_TRIGGER);
                assert(str_trigger);

                st->opts[n].from_value.str_value.str =
                        strdup(cfg_getstr(str_trigger,
                                          C_FROM_VALUE));
                st->opts[n].from_value.str_value.reg = malloc(sizeof(regex_t));
                int ret = 0;
                ret = regcomp(st->opts[n].from_value.str_value.reg,
                              st->opts[n].from_value.str_value.str,
                              REG_EXTENDED | REG_NOSUB);
                if (ret < 0) {
                    char err_text[1024];
                    regerror(ret, &creg, err_text, 1024);
                    slog(SLOG_WARN, "Can't compile regex: %s : %s",
                         st->opts[n].from_value.str_value.str, err_text);
                    valid = false;;
                }
                st->opts[n].to_value.str_value.str =
                        strdup(cfg_getstr(str_trigger,
                                          C_TO_VALUE));
                st->opts[n].to_value.str_value.reg = malloc(sizeof(regex_t));
                ret = regcomp(st->opts[n].to_value.str_value.reg,
                              st->opts[n].to_value.str_value.str,
                              REG_EXTENDED | REG_NOSUB);
                if (ret < 0) {
                    char err_text[1024];
                    regerror(ret, &creg, err_text, 1024);
                    slog(SLOG_WARN, "Can't compile regex: %s : %s",
                         st->opts[n].to_value.str_value.str, err_text);
                    valid = false;;
                }

                st->opts[n].value = get_sane_option_value(st->h, opt);

                if (!valid) {
                    sane_option_value_free(&st->opts[n].from_value);
                    sane_option_value_free(&st->opts[n].to_value);
                    sane_option_value_free(&st->opts[n].value);
                    continue;
                }
            } // type STRING
            else {
                assert(false); // should not happen
            }
            if (n == st->num_of_options_with_scripts) {
                // not found in the list
                // we have a new option to be polled
                st->num_of_options_with_scripts += 1;
            }
        } // foreach option
        // this compiled regex isn't used anymore
        regfree(&creg);
    } // foreach action
}


// thread start funktion
// TODO: refactor, this is awfull long!

static void* sane_poll(void* arg) {
#ifdef CANCEL_TEST
    if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL) < 0) {
        slog(SLOG_ERROR, "pthread_setcancelstate: %s", strerror(errno));
    }
#endif
    sane_thread_t* st = (sane_thread_t*)arg;
    assert(st != NULL);
    slog(SLOG_DEBUG, "sane_poll");
    // we only expect the main thread to handle signals
    sigset_t mask;
    sigfillset(&mask);
    pthread_sigmask(SIG_BLOCK, &mask, NULL);
    
//    static int si = 0; // don't know why this was a static variable -> nonsense in the case of multiple sane_poll threads
    int si = 0;

    // this thread uses the device and the san_thread_t datastructure
    // lock it
    pthread_cleanup_push(sane_thread_cleanup_mutex, ((void*)&st->mutex));
    if (pthread_mutex_lock(&st->mutex) < 0) {
        // if we can't get the mutex, something is heavily wrong!
        slog(SLOG_ERROR, "pthread_mutex_lock: %s", strerror(errno));
        pthread_exit(NULL);
    }
    
    // open the device this thread should poll
    SANE_Status status = SANE_STATUS_INVAL;
    if ((status = sane_open(st->dev->name, &st->h)) != SANE_STATUS_GOOD) {
        slog(SLOG_ERROR, "Can't open device %s: %s", st->dev->name, sane_strstatus(status));
        slog(SLOG_WARN, "abandon polling of %s", st->dev->name);
        pthread_exit(NULL);
    }
    // figure out the number of options this device has
    // option 0 (zero) is guaranteed to exist with the total number of
    // options of that device (including option 0)
    st->num_of_options = 0;
    if ((status = sane_control_option(st->h, 0, SANE_ACTION_GET_VALUE,
                                      &st->num_of_options, 0)) != SANE_STATUS_GOOD) {
        slog(SLOG_ERROR, "Can't get the number of scanner options");
        pthread_exit(NULL);
    }
    if (st->num_of_options == 0) {
        // no options -> nothing to poll
        slog(SLOG_INFO, "No options for device %s", st->dev->name);
        pthread_exit(NULL);
    }
    slog(SLOG_INFO, "found %d options for device %s", st->num_of_options, st->dev->name);

    // allocate an array of options for the  matching actions
    //
    // only one script is possible per option, later matching
    // actions overwrite previous ones

    // initialize the list of matching options
    if (st->opts != NULL) {
        slog(SLOG_ERROR, "possible memory leak: %s, %d", __FILE__, __LINE__);
    }
    st->opts = NULL;
    st->opts = (sane_dev_option_t*) calloc(st->num_of_options,
                                           sizeof(sane_dev_option_t));
    assert(st->opts != NULL);
    for(int i = 0; i < st->num_of_options; i += 1) {
        sane_option_value_init(&st->opts[i].from_value);
        sane_option_value_init(&st->opts[i].to_value);
        sane_option_value_init(&st->opts[i].value);
    }

    // the number of valid entries in the above list
    st->num_of_options_with_scripts = 0;

    // initialize the list of matching functions
    if (st->functions != NULL) {
        slog(SLOG_ERROR, "possible memory leak: %s, %d", __FILE__, __LINE__);
    }
    st->functions = NULL;
    st->functions = (sane_dev_function_t*) calloc(st->num_of_options,
                                                  sizeof(sane_dev_function_t));
    assert(st->functions != NULL);
    for(int i = 0; i < st->num_of_options; i += 1) {
        st->functions[i].number = 0;
        st->functions[i].env = NULL;
    }
    // the number of valid entries in the above list
    st->num_of_options_with_functions = 0;

    // find out the functions and actions
    // get the global sconfig section
    cfg_t* cfg_sec_global = NULL;
    cfg_sec_global = cfg_getsec(cfg, C_GLOBAL);
    assert(cfg_sec_global);

    // find the global actions
    sane_find_matching_options(st, cfg_sec_global);

    // find the global functions
    sane_find_matching_functions(st, cfg_sec_global);
    
    // find (if any) device specifc sections
    // these override global definitions, if any
    int local_sections = cfg_size(cfg, C_DEVICE);
    slog(SLOG_DEBUG, "found %d local device sections", local_sections);
    
    for(int loc = 0; loc < local_sections; loc += 1) {
        cfg_t* loc_i = cfg_getnsec(cfg, C_DEVICE, loc);
        assert(loc_i != NULL);

        // get the filter-regex from the config-file
        const char* loc_regex = cfg_getstr(loc_i, C_FILTER);
        assert(loc_regex != NULL);

        const char* title = cfg_title(loc_i);
        if (title == NULL) {
            title = "(none)";
        }
        // compile the filter-regex
        slog(SLOG_INFO, "checking device section %s with filter: %s",
             title, loc_regex);
        regex_t creg;
        int ret = regcomp(&creg, loc_regex, REG_EXTENDED | REG_NOSUB);
        if (ret < 0) {
            char err_text[1024];
            regerror(ret, &creg, err_text, 1024);
            slog(SLOG_WARN, "Can't compile regex: %s : %s", loc_regex, err_text);
            continue;
        }
        // compare the regex against the device name
        if (regexec(&creg, st->dev->name, 0, NULL, 0) == 0) {
            // match
            int loc_actions = cfg_size(loc_i, C_ACTION);
            slog(SLOG_INFO, "found %d local action for device %s [%s]",
                 loc_actions, st->dev->name, title);
            // get the local actions for this device
            sane_find_matching_options(st, loc_i);
            // get the local functions for this device
            sane_find_matching_functions(st, loc_i);
        }
        regfree(&creg);
    } // foreach local section
    
    int timeout = cfg_getint(cfg_sec_global, C_TIMEOUT);
    if (timeout <= 0) {
        timeout = C_TIMEOUT_DEF;
    }
    slog(SLOG_DEBUG, "timeout: %d ms", timeout);
    
    slog(SLOG_DEBUG, "Start the polling for device %s", st->dev->name);
    while(true) {
        slog(SLOG_DEBUG, "polling thread for %s, before cancellation point", st->dev->name);
#ifdef CANCEL_TEST
        if (pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL) < 0) {
            slog(SLOG_ERROR, "pthread_setcancelstate: %s", strerror(errno));
        }
#endif
        // special cancellation point
        pthread_testcancel();

#ifdef CANCEL_TEST
    if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL) < 0) {
        slog(SLOG_ERROR, "pthread_setcancelstate: %s", strerror(errno));
    }
#endif
    slog(SLOG_DEBUG, "polling thread for %s, after cancellation point", st->dev->name);

    slog(SLOG_DEBUG, "polling device %s", st->dev->name);

        for(si = 0; si < st->num_of_options_with_scripts; si += 1) {
            const SANE_Option_Descriptor* odesc = NULL;
            odesc = sane_get_option_descriptor(st->h, st->opts[si].number);
            assert(odesc);

            if (st->opts[si].script != NULL) {
                if (strlen(st->opts[si].script) <= 0) {
                    slog(SLOG_WARN, "No valid script for option %s for device %s",
                         odesc->name, st->dev->name);
                    continue;
                }
            }
            else {
                slog(SLOG_WARN, "No script for option %s for device %s",
                     odesc->name, st->dev->name);
                continue;
            }
            assert(st->opts[si].script != NULL);
            assert(strlen(st->opts[si].script) > 0);

            sane_opt_value_t value;
            sane_option_value_init(&value);
            // push the cleanup-handler to free the value storage
            pthread_cleanup_push(sane_thread_cleanup_value, &value);

            // get the actual value
            // but don't query an option twice or more (see config multiple_actions)
            // because this may reset the values and no other value changes can be
            // detected
            int o = 0;
            bool gotAlready = false;
            for(o = 0; o < si; o += 1) {
                if (st->opts[o].number == st->opts[si].number) {
                    gotAlready = true;
                    break;
                }
            }
            if (!gotAlready) {
                // first query of option with this number
                value = get_sane_option_value(st->h, st->opts[si].number);
            }
            else {
                // additional query, so copy the value
                slog(SLOG_INFO, "got the value already -> copy");
                // found: copy the value
                slog(SLOG_DEBUG, "copy the value of option %d", st->opts[o].number);
                value.num_value = st->opts[o].value.num_value;
                if (st->opts[o].value.str_value.str != NULL) {
                    value.str_value.str = strdup(st->opts[o].value.str_value.str);
                    assert(value.str_value.str != NULL);
                }
            }

            slog(SLOG_INFO, "checking option %s number %d (%d) for device %s: value: %d",
                 odesc->name, st->opts[si].number, si,
                 st->dev->name, value);

            if ((odesc->type == SANE_TYPE_BOOL) || (odesc->type == SANE_TYPE_INT) ||
                    (odesc->type == SANE_TYPE_FIXED) || (odesc->type == SANE_TYPE_BUTTON)) {
                if ((st->opts[si].from_value.num_value == st->opts[si].value.num_value) &&
                        (st->opts[si].to_value.num_value == value.num_value)) {
                    slog(SLOG_DEBUG, "value trigger: numerical");
                    st->triggered = true;
                    st->triggered_option = si;
                    // we need to trigger all waiting threads
                    if (pthread_cond_broadcast(&st->cv) < 0) {
                        slog(SLOG_ERROR, "pthread_cond_broadcats: this shouln't happen");
                    }
                }
            }
            else if (odesc->type == SANE_TYPE_STRING) {
                if ((regexec(st->opts[si].from_value.str_value.reg,
                             st->opts[si].value.str_value.str, 0, NULL, 0) == 0) &&
                        (regexec(st->opts[si].to_value.str_value.reg,
                                 value.str_value.str, 0, NULL, 0) == 0)) {
                    slog(SLOG_DEBUG, "value trigger: string");
                    st->triggered = true;
                    st->triggered_option = si;
                    // we need to trigger all waiting threads
                    if (pthread_cond_broadcast(&st->cv) < 0) {
                        slog(SLOG_ERROR, "pthread_cond_broadcats: this shouln't happen");
                    }
                }
            }
            else {
                assert(false);
            }
            // free the previous allocated value
            sane_option_value_free(&st->opts[si].value);

            // pass the responsibility to free the value to the main
            // thread, if this thread gets canceled
            if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL) < 0) {
                slog(SLOG_ERROR, "pthread_setcancelstate: %s", strerror(errno));
            }
            st->opts[si].value = value;
            pthread_cleanup_pop(0);
            if (pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL) < 0) {
                slog(SLOG_ERROR, "pthread_setcancelstate: %s", strerror(errno));
            }

            // was there a value change?
            if (st->triggered && (st->triggered_option >= 0)) {
                assert(st->triggered_option >= 0); // index into the opts-array
                assert(st->triggered_option < st->num_of_options_with_scripts);

                slog(SLOG_ERROR, "trigger action for %s for device %s with script %s",
                     odesc->name, st->dev->name, st->opts[st->triggered_option].script);

                // prepare the environment for the script to be called

                // number of env-vars =
                // number of found function-options
                // plus the values in the environment-section (2):
                // device, action
                // plus those 4:
                // PATH, PWD, USER, HOME
                // plus the sentinel
                cfg_t* global_envs = cfg_getsec(cfg_sec_global, C_ENVIRONMENT);

                int number_of_envs = st->num_of_options_with_functions + 4 + 2 + 1;
                char** env = calloc(number_of_envs, sizeof(char*));
                for(int e = 0; e < number_of_envs; e += 1) {
                    env[e] = calloc(NAME_MAX + 1, sizeof(char));
                }
                int e = 0;
                for(e = 0; e < st->num_of_options_with_functions; e += 1) {
                    const SANE_Option_Descriptor* fdesc = NULL;
                    fdesc = sane_get_option_descriptor(st->h,
                                                       st->functions[e].number);
                    assert(fdesc);

                    // check if the function-option is the same
                    // as a action-option. If so, use the
                    // action-option value instead of re-get the same
                    // option value, because it is (may be) reset
                    // after the query by the backend

                    sane_opt_value_t v;
                    sane_option_value_init(&v);
                    int o = 0;
                    for(o = 0; o < st->num_of_options_with_scripts; o += 1) {
                        if (st->opts[o].number == st->functions[e].number) {
                            break;
                        }
                    }
                    if (o == st->num_of_options_with_scripts) {
                        // not found: query the value
                        v = get_sane_option_value(st->h, st->functions[e].number);
                    }
                    else {
                        slog(SLOG_DEBUG, "don't re-get the value");
                    }
                    if ((fdesc->type == SANE_TYPE_BOOL) || (fdesc->type == SANE_TYPE_INT) ||
                            (fdesc->type == SANE_TYPE_FIXED) || (odesc->type == SANE_TYPE_BUTTON)) {
                        snprintf(env[e], NAME_MAX, "%s=%lu", st->functions[e].env,
                                 v.num_value);
                        slog(SLOG_DEBUG, "setting env: %s", env[e]);
                    }
                    else if (fdesc->type == SANE_TYPE_STRING) {
                        snprintf(env[e], NAME_MAX, "%s=%s", st->functions[e].env,
                                 v.str_value.str);
                        slog(SLOG_DEBUG, "setting env: %s", env[e]);
                    }
                    else {
                        assert(false);
                    }
                    sane_option_value_free(&v);
                }
                const char* ev = "PATH";
                if (getenv(ev) != NULL) {
                    snprintf(env[e], NAME_MAX, "%s=%s", ev, getenv(ev));
                    slog(SLOG_DEBUG, "setting env: %s", env[e]);
                    e += 1;
                }
                else {
                    snprintf(env[e], NAME_MAX, "%s=%s", ev, "/usr/sbin:/usr/bin:/sbin:/bin");
                    slog(SLOG_DEBUG, "No PATH, setting env: %s", env[e]);
                    e += 1;
                }
                ev = "PWD";
                if (getenv(ev) != NULL) {
                    snprintf(env[e], NAME_MAX, "%s=%s", ev, getenv(ev));
                    slog(SLOG_DEBUG, "setting env: %s", env[e]);
                    e += 1;
                }
                else {
                    char buf[PATH_MAX+1];
                    char* ptr = getcwd(buf, PATH_MAX);
                    if (!ptr) {
                        slog(SLOG_ERROR, "can't get pwd");
                    }
                    else {
                        assert(ptr);
                        snprintf(env[e], NAME_MAX, "%s=%s", ev, ptr);
                        slog(SLOG_DEBUG, "No PWD, setting env: %s", env[e]);
                        e += 1;
                    }
                }
                ev = "USER";
                if (getenv(ev) != NULL) {
                    snprintf(env[e], NAME_MAX, "%s=%s", ev, getenv(ev));
                    slog(SLOG_DEBUG, "setting env: %s", env[e]);
                    e += 1;
                }
                else {
                    struct passwd* pwd = NULL;
                    pwd = getpwuid(geteuid());
                    assert(pwd);
                    snprintf(env[e], NAME_MAX, "%s=%s", ev, pwd->pw_name);
                    slog(SLOG_DEBUG, "No USER, setting env: %s", env[e]);
                    e += 1;
                }
                ev = "HOME";
                if (getenv(ev) != NULL) {
                    snprintf(env[e], NAME_MAX, "%s=%s", ev, getenv(ev));
                    slog(SLOG_DEBUG, "setting env: %s", env[e]);
                    e += 1;
                }
                else {
                    struct passwd* pwd = 0;
                    pwd = getpwuid(geteuid());
                    assert(pwd);
                    snprintf(env[e], NAME_MAX, "%s=%s", ev, pwd->pw_dir);
                    slog(SLOG_DEBUG, "No HOME, setting env: %s", env[e]);
                    e += 1;
                }
                ev = cfg_getstr(global_envs, C_DEVICE);
                if (ev != NULL) {
                    snprintf(env[e], NAME_MAX, "%s=%s", ev, st->dev->name);
                    slog(SLOG_DEBUG, "setting env: %s", env[e]);
                    e += 1;
                }
                ev = cfg_getstr(global_envs, C_ACTION);
                if (ev != NULL) {
                    snprintf(env[e], NAME_MAX, "%s=%s", ev,
                             st->opts[st->triggered_option].action_name);
                    slog(SLOG_DEBUG, "setting env: %s", env[e]);
                    e += 1;
                }
                env[e] = NULL;
                assert(e == number_of_envs-1);

                // sendout an dbus-signal with all the values as
                // arguments
                dbus_send_signal(SCANBD_DBUS_SIGNAL_SCAN_BEGIN, st->dev->name);

                //dbus_send_signal_argv_async(SCANBD_DBUS_SIGNAL_TRIGGER, env);
                dbus_send_signal_argv(SCANBD_DBUS_SIGNAL_TRIGGER, env);
                // the action-script will use the device,
                // so we have to release the device
                sane_close(st->h);
                st->h = NULL;

                assert(st->triggered_option >= 0);
                assert(st->opts[st->triggered_option].script);
                assert(strlen(st->opts[st->triggered_option].script) > 0);

                // need to copy the values because we leave the
                // critical section
                // While doing so, convert the script to an absolute path
                // int triggered_option = st->triggered_option;
          
                char *script_abs = 
                     make_script_path_abs(st->opts[st->triggered_option].script);
                
                assert(script_abs);

                // leave the critical section
                if (pthread_mutex_unlock(&st->mutex) < 0) {
                    // if we can't unlock the mutex, something is heavily wrong!
                    slog(SLOG_ERROR, "pthread_mutex_unlock: %s", strerror(errno));
                    pthread_exit(NULL);
                }

                if (strcmp(script_abs, SCANBD_NULL_STRING) != 0) {

                    assert(timeout > 0);
                    usleep(timeout * 1000); //ms

                    pid_t cpid;
                    if ((cpid = fork()) < 0) {
                        slog(SLOG_ERROR, "Can't fork: %s", strerror(errno));
                    }
                    else if (cpid > 0) { // parent
                        slog(SLOG_INFO, "waiting for child: %s", script_abs);
                        int status;
                        if (waitpid(cpid, &status, 0) < 0) {
                            slog(SLOG_ERROR, "waitpid: %s", strerror(errno));
                        }
                        if (WIFEXITED(status)) {
                            slog(SLOG_INFO, "child %s exited with status: %d",
                                 script_abs, WEXITSTATUS(status));
                        }
                        if (WIFSIGNALED(status)) {
                            slog(SLOG_INFO, "child %s signaled with signal: %d",
                                 script_abs, WTERMSIG(status));
                        }
                    }
                    else { // child
                        uid_t euid = geteuid();
                        uid_t egid = getegid();
                        if (seteuid(0) < 0) {
                            slog(SLOG_DEBUG, "Can't seteuid root: %s", strerror(errno));
                            exit(EXIT_FAILURE);
                        } 
                        if (setegid(0) < 0) {
                            slog(SLOG_DEBUG, "Can't setegid root: %s", strerror(errno));
                            exit(EXIT_FAILURE);
                        } 
                        slog(SLOG_DEBUG, "setgid to gid=%d", egid);
                        if (setgid(egid) < 0) {
                            slog(SLOG_DEBUG, "Can't setgid for gid=%d: %s", egid, strerror(errno));
                            exit(EXIT_FAILURE);
                        } 
                        slog(SLOG_DEBUG, "setuid to uid=%d", euid);
                        if (setuid(euid) < 0) {
                            slog(SLOG_DEBUG, "Can't setuid for uid=%d : %s", euid, strerror(errno));
                            exit(EXIT_FAILURE);
                        } 
                        
                        slog(SLOG_DEBUG, "exec for %s", script_abs);
                        if (access(script_abs, F_OK | X_OK) < 0) {
                            slog(SLOG_ERROR, "access: %s", strerror(errno));
                        }
                        struct stat stat_buf;
                        if (stat(script_abs, &stat_buf) < 0) {
                            slog(SLOG_ERROR, "stat: %s", strerror(errno));
                        }
                        else {
                            slog(SLOG_DEBUG, "octal mode for %s: %lo", script_abs, stat_buf.st_mode);
                            slog(SLOG_DEBUG, "file uid: %ld, file gid: %ld", stat_buf.st_uid, stat_buf.st_gid);
                        }
                        if (execle(script_abs, script_abs, NULL, env) < 0) {
                            slog(SLOG_ERROR, "execlp: %s", strerror(errno));
                        }
                        exit(EXIT_FAILURE); // not reached
                    }
                } // script_abs == SCANBD_NULL_STRING

                assert(script_abs != NULL);
                free(script_abs);

                // free (last element is the sentinel!)
                assert(env != NULL);
                for(int e = 0; e < number_of_envs - 1; e += 1) {
                    assert(env[e] != NULL);
                    free(env[e]);
                }
                free(env);

                // enter the critical section
                if (pthread_mutex_lock(&st->mutex) < 0) {
                    // if we can't get the mutex, something is heavily wrong!
                    slog(SLOG_ERROR, "pthread_mutex_lock: %s", strerror(errno));
                    pthread_exit(NULL);
                }

                st->triggered = false;
                st->triggered_option = -1; // invalid
                // we need to trigger all waiting threads
                if (pthread_cond_broadcast(&st->cv) < 0) {
                    slog(SLOG_ERROR, "pthread_cond_broadcats: this shouln't happen");
                }

                // leave the critical section
                if (pthread_mutex_unlock(&st->mutex) < 0) {
                    // if we can't release the mutex, something is heavily wrong!
                    slog(SLOG_ERROR, "pthread_mutex_unlock: %s", strerror(errno));
                    pthread_exit(NULL);
                }
                // sleep the timeout to settle devices, necessary?
                usleep(timeout * 1000); //ms

                // send out the debus signal
                dbus_send_signal(SCANBD_DBUS_SIGNAL_SCAN_END, st->dev->name);

                // enter the critical section
                if (pthread_mutex_lock(&st->mutex) < 0) {
                    // if we can't get the mutex, something is heavily wrong!
                    slog(SLOG_ERROR, "pthread_mutex_lock: %s", strerror(errno));
                    pthread_exit(NULL);
                }

                slog(SLOG_DEBUG, "reopen device %s", st->dev->name);
                if ((status = sane_open(st->dev->name, &st->h)) != SANE_STATUS_GOOD) {
                    slog(SLOG_ERROR, "Can't open device %s, %s",
                         st->dev->name, sane_strstatus(status));
                    if (status == SANE_STATUS_ACCESS_DENIED) {
                        slog(SLOG_WARN, "abandon polling of %s", st->dev->name);
                        pthread_exit(NULL);
                    }
                }
            } // if triggered
        } // foreach option

        // release the mutex

        // because pthread_cleanup_pop is a macro we can't use it here
        // pthread_cleanup_pop(1);
        if (pthread_mutex_unlock(&st->mutex) < 0) {
            // if we can't unlock the mutex, something is heavily wrong!
            slog(SLOG_ERROR, "pthread_mutex_unlock: %s", strerror(errno));
            pthread_exit(NULL);
        }

        // sleep the polling timeout
        usleep(timeout * 1000); //ms

        // regain the mutex
        // because pthread_cleanup_push is a macro we can't use it here
//         pthread_cleanup_push(sane_thread_cleanup_mutex, ((void*)&st->mutex));
        if (pthread_mutex_lock(&st->mutex) < 0) {
            // if we can't get the mutex, something is heavily wrong!
            slog(SLOG_ERROR, "pthread_mutex_lock: %s", strerror(errno));
            pthread_exit(NULL);
        }
    }
    pthread_cleanup_pop(1); // release the mutex
    pthread_exit(NULL);
}

// helper to trigger a specified action from another thread
// (e.g. dbus) via an action number
void sane_trigger_action(int number_of_dev, int action) {
    assert(number_of_dev >= 0);
    assert(action >= 0);
    slog(SLOG_DEBUG, "sane_trigger_action device=%d, action=%d", number_of_dev, action);

    if (pthread_mutex_lock(&sane_mutex) < 0) {
        slog(SLOG_ERROR, "pthread_mutex_lock: %s", strerror(errno));
        return;
    }
    if (num_devices <= 0) {
        slog(SLOG_WARN, "No devices at all");
        goto cleanup_sane;
    }
    if (number_of_dev >= num_devices) {
        slog(SLOG_WARN, "No such device number %d", number_of_dev);
        goto cleanup_sane;
    }

    while(sane_poll_threads == NULL) {
        // no devices actually polling
        slog(SLOG_WARN, "No polling at the moment, waiting ...");
        if (pthread_cond_wait(&sane_cv, &sane_mutex) < 0) {
            slog(SLOG_ERROR, "pthread_cond_wait: ", strerror(errno));
            goto cleanup_sane;
        }
    }
    assert(sane_poll_threads != NULL);
    sane_thread_t* st = &sane_poll_threads[number_of_dev];
    assert(st != NULL);
    
    // this thread uses the device and the sane_thread_t datastructure
    // lock it
    if (pthread_mutex_lock(&st->mutex) < 0) {
        slog(SLOG_ERROR, "pthread_mutex_lock: %s", strerror(errno));
        goto cleanup_sane;
    }

    if (action >= st->num_of_options_with_scripts) {
        slog(SLOG_WARN, "No such action %d for device number %d", action, number_of_dev);
        goto cleanup_dev;
    }

    while(st->triggered == true) {
        slog(SLOG_DEBUG, "sane_trigger_action: an action is active, waiting ...");
        if (pthread_cond_wait(&st->cv, &st->mutex) < 0) {
            slog(SLOG_ERROR, "pthread_cond_wait: %s", strerror(errno));
            goto cleanup_dev;
        }
    }
    
    slog(SLOG_DEBUG, "sane_trigger_action: an action is active, waiting ...");

    st->triggered = true;
    st->triggered_option = action;
    // we need to trigger all waiting threads
    if (pthread_cond_broadcast(&st->cv) < 0) {
        slog(SLOG_ERROR, "pthread_cond_broadcats: this shouln't happen");
    }

cleanup_dev:
    if (pthread_mutex_unlock(&st->mutex) < 0) {
        slog(SLOG_ERROR, "pthread_mutex_unlock: %s", strerror(errno));
    }
cleanup_sane:
    if (pthread_mutex_unlock(&sane_mutex) < 0) {
        slog(SLOG_ERROR, "pthread_mutex_unlock: %s", strerror(errno));
    }
    return;
}

void start_sane_threads(void) {
    slog(SLOG_DEBUG, "start_sane_threads");

    if (pthread_mutex_lock(&sane_mutex) < 0) {
        // if we can't get the mutex, something is heavily wrong!
        slog(SLOG_ERROR, "pthread_mutex_lock: %s", strerror(errno));
        return;
    }
    
    if (sane_poll_threads != NULL) {
        // if there are active threads kill them
        stop_sane_threads();
    }
    // allocate the thread list
    assert(sane_poll_threads == NULL);
    
    if (num_devices == 0) {
        slog(SLOG_ERROR, "no devices, not starting any polling thread");
        goto cleanup;
    } 
    sane_poll_threads = (sane_thread_t*) calloc(num_devices, sizeof(sane_thread_t));
    if (sane_poll_threads == NULL) {
        slog(SLOG_ERROR, "Can't allocate memory for polling threads");
        goto cleanup;
    }
    // starting for each device a seperate thread
    for(int i = 0; i < num_devices; i += 1) {
        slog(SLOG_DEBUG, "Starting poll thread for %s", sane_device_list[i]->name);
        sane_poll_threads[i].tid = 0;
        sane_poll_threads[i].dev = sane_device_list[i];
        sane_poll_threads[i].h = 0;
        sane_poll_threads[i].opts = NULL;
        sane_poll_threads[i].functions = NULL;
        sane_poll_threads[i].num_of_options = 0;
        sane_poll_threads[i].triggered = false;
        sane_poll_threads[i].triggered_option = -1;
        sane_poll_threads[i].num_of_options_with_scripts = 0;
        sane_poll_threads[i].num_of_options_with_functions = 0;

        if (pthread_mutex_init(&sane_poll_threads[i].mutex, NULL) < 0) {
            slog(SLOG_ERROR, "pthread_mutex_init: should not happen");
        }
        if (pthread_cond_init(&sane_poll_threads[i].cv, NULL) < 0) {
            slog(SLOG_ERROR, "pthread_cond_init: should not happen");
        }
        if (pthread_create(&sane_poll_threads[i].tid, NULL, sane_poll,
                           (void*)&sane_poll_threads[i]) < 0) {
            slog(SLOG_ERROR, "Can't start sane_poll_thread: %s", strerror(errno));
            exit(EXIT_FAILURE);
        }
        slog(SLOG_DEBUG, "Thread started for device %s", sane_device_list[i]->name);
    }
    if (pthread_cond_broadcast(&sane_cv)) {
        slog(SLOG_ERROR, "pthread_cond_broadcast: %s", strerror(errno));
    }
cleanup:
    if (pthread_mutex_unlock(&sane_mutex) < 0) {
        // if we can't unlock the mutex, something is heavily wrong!
        slog(SLOG_ERROR, "pthread_mutex_unlock: %s", strerror(errno));
        return;
    }
}

// stops all sane polling threads

void stop_sane_threads(void) {
    slog(SLOG_DEBUG, "stop_sane_threads");

    if (pthread_mutex_lock(&sane_mutex) < 0) {
        // if we can't get the mutex, something is heavily wrong!
        slog(SLOG_ERROR, "pthread_mutex_lock: %s", strerror(errno));
        return;
    }
    
    if (sane_poll_threads == NULL) {
        // we don't have any active threads
        slog(SLOG_DEBUG, "stop_sane_threads: nothing to stop");
        goto cleanup;
    }
    // sending cancel request to all threads
    for(int i = 0; i < num_devices; i += 1) {
        if (pthread_mutex_lock(&sane_poll_threads[i].mutex) < 0) {
            slog(SLOG_ERROR, "pthread_mutex_lock: %s", strerror(errno));
        }
        while(sane_poll_threads[i].triggered == true) {
            slog(SLOG_DEBUG, "stop_sane_threads: an action is active, waiting ...");

            if (pthread_cond_wait(&sane_poll_threads[i].cv,
                                  &sane_poll_threads[i].mutex) < 0) {
                slog(SLOG_ERROR, "pthread_cond_wait: %s", strerror(errno));
            }
        }
        if (pthread_mutex_unlock(&sane_poll_threads[i].mutex) < 0) {
            slog(SLOG_ERROR, "pthread_mutex_lock: %s", strerror(errno));
        }

        slog(SLOG_DEBUG, "stopping poll thread for device %s", (*(sane_device_list + i))->name);
        if (pthread_cancel(sane_poll_threads[i].tid) < 0) {
            if (errno == ESRCH) {
                slog(SLOG_ERROR, "poll thread for device %s was already cancelled");
            }
            else {
                slog(SLOG_ERROR, "unknown error from pthread_cancel: %s", strerror(errno));
            }
        }
    }
    // waiting for all threads to vanish
    for(int i = 0; i < num_devices; i += 1) {
        slog(SLOG_DEBUG, "waiting for poll thread for device %s",
             (*(sane_device_list + i))->name);
        // joining all threads to prevent memory leaks
        if (pthread_join(sane_poll_threads[i].tid, NULL) < 0) {
            slog(SLOG_ERROR, "pthread_join: %s", strerror(errno));
        }
        sane_poll_threads[i].tid = 0;
        // close the associated device of the thread
        slog(SLOG_DEBUG, "closing device %s", sane_poll_threads[i].dev->name);
        if (sane_poll_threads[i].h != NULL) {
            sane_close(sane_poll_threads[i].h);
            sane_poll_threads[i].h = NULL;
        }
        if (sane_poll_threads[i].opts) {
            slog(SLOG_DEBUG, "freeing opt resources for device %s thread",
                 sane_poll_threads[i].dev->name);
            // free the matching options list of that device / threads
            for (int k = 0; k < sane_poll_threads[i].num_of_options; k += 1) {
                sane_option_value_free(&sane_poll_threads[i].opts[k].from_value);
                sane_option_value_free(&sane_poll_threads[i].opts[k].to_value);
                sane_option_value_free(&sane_poll_threads[i].opts[k].value);
            }
            free(sane_poll_threads[i].opts);
            sane_poll_threads[i].opts = NULL;
        }
        if (sane_poll_threads[i].functions) {
            slog(SLOG_DEBUG, "freeing function resources for device %s thread",
                 sane_poll_threads[i].dev->name);
            free(sane_poll_threads[i].functions);
            sane_poll_threads[i].functions = NULL;
        }

        if (pthread_cond_destroy(&sane_poll_threads[i].cv) < 0) {
            slog(SLOG_ERROR, "pthread_cond_destroy: %s", strerror(errno));
        }
        if (pthread_mutex_destroy(&sane_poll_threads[i].mutex) < 0) {
            slog(SLOG_ERROR, "pthread_mutex_destroy: %s", strerror(errno));
        }
    }
    // free the thread list
    free(sane_poll_threads);
    sane_poll_threads = NULL;
    // no threads active anymore
    if (pthread_cond_broadcast(&sane_cv)) {
        slog(SLOG_ERROR, "pthread_cond_broadcast: %s", strerror(errno));
    }
cleanup:
    if (pthread_mutex_unlock(&sane_mutex) < 0) {
        // if we can't unlock the mutex, something is heavily wrong!
        slog(SLOG_ERROR, "pthread_mutex_unlock: %s", strerror(errno));
        return;
    }
}
