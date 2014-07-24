#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <time.h>
#include <sys/resource.h>
#include <string.h>

#include <libwandevent.h>

#include "config.h"
#include "schedule.h"
#include "watchdog.h"
#include "test.h"
#include "debug.h"
#include "nametable.h"
#include "modules.h"
#include "global.h" /* hopefully temporary, just to get source iface/address */
#include "ampresolv.h"



/*
 * Combine the test parameters with any from the test set up function and
 * apply them to the proper test binary as provided by the test registration.
 * Run the test callback function and let it do its thing.
 */
static void run_test(const test_schedule_item_t * const item) {
    char *argv[MAX_TEST_ARGS];
    uint32_t argc = 0;
    uint32_t offset;
    test_t *test;
    resolve_dest_t *resolve;
    struct addrinfo *addrlist = NULL;
    struct addrinfo **destinations = NULL;
    int total_resolve_count = 0;

    assert(item);
    assert(item->test_id < AMP_TEST_LAST);
    assert(amp_tests[item->test_id]);
    assert((item->dest_count + item->resolve_count) > 0);

    /*
     * seed the random number generator, has to be after the fork() or each
     * new process inherits exactly the same one and always returns the first
     * element in the sequence
     */
    srandom(time(NULL));

    test = amp_tests[item->test_id];
    argv[argc++] = test->name;

    /* set the outgoing interface if configured at the global level */
    if ( vars.interface != NULL ) {
        argv[argc++] = "-I";
        argv[argc++] = vars.interface;
    }

    /* set the outgoing source v4 address if configured at the global level */
    if ( vars.sourcev4 != NULL ) {
        argv[argc++] = "-4";
        argv[argc++] = vars.sourcev4;
    }

    /* set the outgoing source v6 if configured at the global level */
    if ( vars.sourcev6 != NULL ) {
        argv[argc++] = "-6";
        argv[argc++] = vars.sourcev6;
    }

    /* add in any of the test parameters from the schedule file */
    if ( item->params != NULL ) {
	for ( offset=0; item->params[offset] != NULL; offset++ ) {
	    argv[argc++] = item->params[offset];
	}
    }

    /* null terminate the list before we give it to the main test function */
    argv[argc] = NULL;

    Log(LOG_DEBUG, "Running test: %s to %d/%d destinations:\n", test->name,
	    item->dest_count, item->resolve_count);

    /* create the destination list for the test if there are fixed targets */
    if ( item->dest_count > 0 ) {
	destinations = malloc(sizeof(struct addrinfo*) * item->dest_count);

	/* copy all currently resolved destination pointers as a block */
	memcpy(destinations, item->dests,
		sizeof(struct addrinfo*) * item->dest_count);
    }

    /* resolve any names that need to be done at test run time */
    if ( item->resolve != NULL ) {
	struct addrinfo *tmp;
        int resolver_fd;

	Log(LOG_DEBUG, "test has destinations to resolve!\n");

        /* connect to the local amp resolver/cache */
        if ( (resolver_fd = amp_resolver_connect(vars.nssock)) < 0 ) {
            Log(LOG_ALERT, "TODO tidy up nicely after failing resolving");
            assert(0);
        }

        /* add all the names that we need to resolve */
        for ( resolve=item->resolve; resolve != NULL; resolve=resolve->next ) {
            amp_resolve_add_new(resolver_fd, resolve);
        }

        /* get the list of all the addresses the names resolved to (blocking) */
        addrlist = amp_resolve_get_list(resolver_fd);

        /* create the destination list from all the resolved addresses */
        for ( tmp = addrlist; tmp != NULL; tmp = tmp->ai_next ) {
            destinations = realloc(destinations,
                    (item->dest_count + total_resolve_count + 1) *
                    sizeof(struct addrinfo));
            destinations[item->dest_count + total_resolve_count] = tmp;
            total_resolve_count++;
        }
    }

    Log(LOG_DEBUG, "Final destination count = %d\n",
	    item->dest_count + total_resolve_count);

    /* only perform the test if there are actually destinations to test to */
    if ( item->dest_count + total_resolve_count > 0 ) {
	//Log(LOG_DEBUG, "dest%d: %s\n", offset,
	//	    address_to_name(item->dests[offset]));

	for ( offset = 0; offset<argc; offset++ ) {
	    Log(LOG_DEBUG, "arg%d: %s\n", offset, argv[offset]);
	}

        /* TODO set up any servers that need to be setup? or allow the tests
         * to do that themselves? We've already forked so we aren't holding
         * anyone up, but who makes more sense to control this?
         */

	/* actually run the test */
	test->run_callback(argc, argv, item->dest_count + total_resolve_count,
		destinations);

	/* free any destinations that we looked up just for this test */
        amp_resolve_freeaddr(addrlist);

	/* just free the temporary list of pointers, leave the actual data */
	if ( destinations != NULL ) {
	    free(destinations);
	}
    }

    /* done running the test, exit */
    exit(0);
}



/*
 * The ELF binary layout means we should have all of the command line
 * arguments and the environment all contiguous in the stack. We can take
 * over all of that space and replace it with whatever program name or
 * description that we want, and relocate the environment to a new location.
 *
 * This approach makes it not portable, but for now we will just make it work
 * with linux. Other operating systems appear to be much smarter than linux
 * anyway and have setproctitle(). We also won't bother saving the original
 * argv array, we shouldn't need it again.
 *
 * See:
 * postgresl-9.3.4/src/backend/utils/misc/ps_status.c
 * util-linux-2.24/lib/setproctitle.c
 *
 * Note:
 * Could maybe also use prctl(), but that only sets the name that top shows
 * by default and is limited to 16 characters (probably won't fit an ampname).
 */
static void set_proc_name(char *testname) {
    char *end;
    int buflen;
    int i;
    char **argv;
    int argc;
    extern char **environ;

    Log(LOG_DEBUG, "Setting name of process %d to '%s: %s %s'", getpid(),
            PACKAGE, vars.ampname, testname);

    /*
     * We have as much space to use as there are contiguous arguments. Every
     * argument should be contiguous, but I guess it's possible that they
     * aren't?
     */
    argc = vars.argc;
    argv = vars.argv;
    end = argv[0] + strlen(argv[0]);
    for ( i = 1; i < argc; i++ ) {
        if ( end + 1 == argv[i] ) {
            end = argv[i] + strlen(argv[i]);
        } else {
            /* not contiguous, stop looking */
            break;
        }
    }

    /*
     * We can also take over any contiguous space used for environment
     * strings if they directly follow the arguments, later making a new
     * environment elsewhere.
     */
    if ( i == argc ) {
        char **env;

        for ( i = 0; environ[i] != NULL; i++ ) {
            if ( end + 1 == environ[i] ) {
                end = environ[i] + strlen(environ[i]);
            } else {
                /* not contiguous, but keep counting environment size */
            }
        }

        /* if we found space we want to use, make a new environment */
        env = (char **) malloc((i + 1) * sizeof(char *));
        for ( i = 0; environ[i] != NULL; i++ ) {
            env[i] = strdup(environ[i]);
        }
        /* null terminate the environment variable array */
        env[i] = NULL;

        /*
         * If we wanted to be really good we could keep a reference to
         * this so we can free it when the test ends, but it's going to
         * get freed anyway.
         */
        environ = env;
    }

    /* we can use as much space as we have contiguous memory */
    buflen = end - argv[0];

    /*
     * Null the rest of the arguments so we don't get pointers to random
     * parts of the new process name.
     */
    for ( i = 1; i < argc; i++ ) {
        argv[i] = NULL;
    }

    /* put our new name at the front of argv[0] and null the rest of it */
    snprintf(argv[0], buflen-1, "%s: %s %s", PACKAGE, vars.ampname, testname);
    memset(argv[0] + strlen(argv[0]) + 1, 0, buflen - strlen(argv[0]) - 1);

    Log(LOG_DEBUG, "Set name of process %d to '%s'", getpid(), argv[0]);
}



/*
 * Test function to investigate forking, rescheduling, setting maximum
 * execution timers etc.
 * TODO maybe just move the contents of this into run_scheduled_test()?
 */
static void fork_test(wand_event_handler_t *ev_hdl,test_schedule_item_t *item) {
    pid_t pid;
    test_t *test;

    assert(item);
    assert(item->test_id < AMP_TEST_LAST);
    assert(amp_tests[item->test_id]);

    test = amp_tests[item->test_id];

    /*
     * man fork:
     * "Under Linux, fork() is implemented using copy-on-write pages..."
     * This should mean that we aren't duplicating massive amounts of memory
     * unless we are modifying it. We shouldn't be modifying it, so should be
     * fine.
     */
    if ( (pid = fork()) < 0 ) {
	perror("fork");
	return;
    } else if ( pid == 0 ) {
	/* child, prepare the environment and run the test functions */
	//struct rlimit cpu_limits;
	//cpu_limits.rlim_cur = 60;
	//cpu_limits.rlim_max = 60;
	/* XXX if this kills a test, how to distinguish it from the watchdog
	 * doing so? in this case the watchdog timer still needs to be removed
	 */
	//setrlimit(RLIMIT_CPU, &cpu_limits);
	/* TODO prepare environment */

        /* update process name so we can tell what is running */
        set_proc_name(test->name);
	run_test(item);
	Log(LOG_WARNING, "%s test failed to run", test->name);//XXX required?
	exit(1);
    }

    //XXX if the test aborts before we add this, will that cock things up?
    /* schedule the watchdog to kill it if it takes too long */
    add_test_watchdog(ev_hdl, pid, test->max_duration, test->name);
}



/*
 * Start a scheduled test running and reschedule it to run again next interval
 */
void run_scheduled_test(wand_event_handler_t *ev_hdl, void *data) {
    schedule_item_t *item = (schedule_item_t *)data;
    test_schedule_item_t *test_item;
    struct timeval next;

    assert(item->type == EVENT_RUN_TEST);

    test_item = (test_schedule_item_t *)item->data.test;

    Log(LOG_DEBUG, "Running a %s test", amp_tests[test_item->test_id]->name);
    printf("running a %s test at %d\n", amp_tests[test_item->test_id]->name,
	    (int)time(NULL));

    /*
     * run the test as soon as we know what it is, so it happens as close to
     * the right time as we can get it.
     */
    fork_test(item->ev_hdl, test_item);

    /* while the test runs, reschedule it again */
    next = get_next_schedule_time(item->ev_hdl, test_item->repeat,
            test_item->start, test_item->end, MS_FROM_TV(test_item->interval));
    wand_add_timer(ev_hdl, next.tv_sec, next.tv_usec, data, run_scheduled_test);
}
