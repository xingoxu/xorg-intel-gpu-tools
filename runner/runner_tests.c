#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <json.h>

#include "igt.h"
#include "runnercomms.h"

#include "settings.h"
#include "job_list.h"
#include "executor.h"
#include "resultgen.h"

/*
 * NOTE: this test is using a lot of variables that are changed in igt_fixture,
 * igt_subtest_group and igt_subtests blocks but defined outside of them.
 *
 * Such variables have to be either non-local or volatile, otherwise their
 * contents is undefined due to longjmps the framework performs.
 */

static const char testdatadir[] = TESTDATA_DIRECTORY;

/*
 * The total sum of subtests in the tests in runner/testdata/. Note
 * that test binaries without subtests should still be counted as one
 * for this macro.
 */
#define NUM_TESTDATA_SUBTESTS 15
#define NUM_TESTDATA_ABORT_SUBTESTS 9
/* The total number of test binaries in runner/testdata/ */
#define NUM_TESTDATA_BINARIES 8

static const char *igt_get_result(struct json_object *tests, const char* testname)
{
	struct json_object *obj;

	igt_assert(json_object_object_get_ex(tests, testname, &obj));
	igt_assert(json_object_object_get_ex(obj, "result", &obj));

	return json_object_get_string(obj);
}

static void igt_assert_no_result_for(struct json_object *tests, const char* testname)
{
	struct json_object *obj;
	igt_assert(!json_object_object_get_ex(tests, testname, &obj));
}


static void igt_assert_eqstr(const char *one, const char *two)
{
	if (one == NULL && two == NULL)
		return;

	igt_assert_f(one != NULL && two != NULL, "Strings differ (one is NULL): %s vs %s\n", one, two);

	igt_assert_f(!strcmp(one, two), "Strings differ: '%s' vs '%s'\n", one, two);
}

static void debug_print_executions(struct job_list *list)
{
	size_t i;
	int k;

	igt_debug("Executions:\n");
	for (i = 0; i < list->size; i++) {
		struct job_list_entry *entry = &list->entries[i];
		igt_debug(" %s\n", entry->binary);
		for (k = 0; k < entry->subtest_count; ++k) {
			igt_debug("  %s\n", entry->subtests[k]);
		}
	}

}

static char *dump_file(int dirfd, const char *name)
{
	int fd = openat(dirfd, name, O_RDONLY);
	ssize_t s;
	char *buf = malloc(256);

	if (fd < 0) {
		free(buf);
		return NULL;
	}

	s = read(fd, buf, 255);
	close(fd);

	if (s < 0) {
		free(buf);
		return NULL;
	}

	buf[s] = '\0';
	return buf;
}

static void job_list_filter_test(const char *name, const char *filterarg1, const char *filterarg2,
				 size_t expected_normal, size_t expected_multiple)
{
	int multiple;
	struct settings *settings = malloc(sizeof(*settings));

	igt_fixture
		init_settings(settings);

	for (multiple = 0; multiple < 2; multiple++) {
		igt_subtest_f("job-list-filters-%s-%s", name, multiple ? "multiple" : "normal") {
			struct job_list list;
			const char *argv[] = { "runner",
					       "--allow-non-root",
					       /* Ugly but does the trick */
					       multiple ? "--multiple-mode" : "--sync",
					       filterarg1, filterarg2,
					       testdatadir,
					       "path-to-results",
			};
			bool success = false;
			size_t size;

			init_job_list(&list);
			igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));

			success = create_job_list(&list, settings);
			size = list.size;

			if (success)
				debug_print_executions(&list);

			free_job_list(&list);

			igt_assert_f(success, "Job list creation failed\n");
			igt_assert_eq(size, multiple ? expected_multiple : expected_normal);
		}
	}

	igt_fixture {
		clear_settings(settings);
		free(settings);
	}
}

static void clear_directory_fd(int dirfd)
{
	DIR* d;
	struct dirent *dirent;

	d = fdopendir(dirfd);

	if (dirfd < 0 || d == NULL) {
		return;
	}

	while ((dirent = readdir(d)) != NULL) {
		if (strcmp(dirent->d_name, ".") &&
		    strcmp(dirent->d_name, "..")) {
			if (dirent->d_type == DT_REG) {
				unlinkat(dirfd, dirent->d_name, 0);
			} else if (dirent->d_type == DT_DIR) {
				clear_directory_fd(openat(dirfd, dirent->d_name, O_DIRECTORY | O_RDONLY));
				unlinkat(dirfd, dirent->d_name, AT_REMOVEDIR);
			}
		}
	}

	closedir(d);
}

static void clear_directory(char *name)
{
	int dirfd = open(name, O_DIRECTORY | O_RDONLY);
	clear_directory_fd(dirfd);
	rmdir(name);
}

static void assert_settings_equal(struct settings *one, struct settings *two)
{
	/*
	 * Regex lists are not serialized, and thus won't be compared
	 * here.
	 */
	igt_assert_eq(one->abort_mask, two->abort_mask);
	igt_assert_eq_u64(one->disk_usage_limit, two->disk_usage_limit);
	igt_assert_eqstr(one->test_list, two->test_list);
	igt_assert_eqstr(one->name, two->name);
	igt_assert_eq(one->dry_run, two->dry_run);
	igt_assert_eq(one->allow_non_root, two->allow_non_root);
	igt_assert_eq(one->sync, two->sync);
	igt_assert_eq(one->log_level, two->log_level);
	igt_assert_eq(one->overwrite, two->overwrite);
	igt_assert_eq(one->multiple_mode, two->multiple_mode);
	igt_assert_eq(one->inactivity_timeout, two->inactivity_timeout);
	igt_assert_eq(one->per_test_timeout, two->per_test_timeout);
	igt_assert_eq(one->use_watchdog, two->use_watchdog);
	igt_assert_eqstr(one->test_root, two->test_root);
	igt_assert_eqstr(one->results_path, two->results_path);
	igt_assert_eq(one->piglit_style_dmesg, two->piglit_style_dmesg);
	igt_assert_eq(one->dmesg_warn_level, two->dmesg_warn_level);
	igt_assert_eq(one->prune_mode, two->prune_mode);
}

static void assert_job_list_equal(struct job_list *one, struct job_list *two)
{
	size_t i, k;

	igt_assert_eq(one->size, two->size);

	for (i = 0; i < one->size; i++) {
		struct job_list_entry *eone = &one->entries[i];
		struct job_list_entry *etwo = &two->entries[i];

		igt_assert_eqstr(eone->binary, etwo->binary);
		igt_assert_eq(eone->subtest_count, etwo->subtest_count);

		for (k = 0; k < eone->subtest_count; k++) {
			igt_assert_eqstr(eone->subtests[k], etwo->subtests[k]);
		}
	}
}

static void assert_execution_created(int dirfd, const char *name)
{
	int fd;

	igt_assert_f((fd = openat(dirfd, name, O_RDONLY)) >= 0,
		     "Execute didn't create %s\n", name);
	close(fd);
}

static void assert_execution_results_exist(int dirfd)
{
	assert_execution_created(dirfd, "journal.txt");
	assert_execution_created(dirfd, "out.txt");
	assert_execution_created(dirfd, "err.txt");
	assert_execution_created(dirfd, "dmesg.txt");
}

static void write_packet_with_canary(int fd, struct runnerpacket *packet)
{
	uint32_t canary = socket_dump_canary();

	write(fd, &canary, sizeof(canary));
	write(fd, packet, packet->size);

	free(packet);
}

igt_main
{
	struct settings *settings = malloc(sizeof(*settings));

	igt_fixture {
		int i;

		/*
		 * Let's close all the non-standard fds ahead of executing
		 * anything, so we can test for descriptor leakage caused by
		 * any of the igt_runner code-paths exercised here.
		 *
		 * See file-descriptor-leakage subtest at the end.
		 *
		 * Some libraries (looking at you, GnuTLS) may leave fds opened
		 * after the implicitly called library constructor. We don't
		 * have full control over them as they may be dependencies of
		 * our dependencies and may get pulled in if the user's and
		 * distribution's compile/configure/USE are just right.
		 */
		for (i = 3; i < 400; i++)
			close(i);

		init_settings(settings);
	}

	igt_subtest("default-settings") {
		const char *argv[] = { "runner",
				       "--allow-non-root",
				       "test-root-dir",
				       "path-to-results",
		};

		igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));

		igt_assert_eq(settings->abort_mask, 0);
		igt_assert_eq_u64(settings->disk_usage_limit, 0UL);
		igt_assert(!settings->test_list);
		igt_assert_eqstr(settings->name, "path-to-results");
		igt_assert(!settings->dry_run);
		igt_assert_eq(settings->include_regexes.size, 0);
		igt_assert_eq(settings->exclude_regexes.size, 0);
		igt_assert(igt_list_empty(&settings->env_vars));
		igt_assert(!settings->sync);
		igt_assert_eq(settings->log_level, LOG_LEVEL_NORMAL);
		igt_assert(!settings->overwrite);
		igt_assert(!settings->multiple_mode);
		igt_assert_eq(settings->inactivity_timeout, 0);
		igt_assert_eq(settings->per_test_timeout, 0);
		igt_assert_eq(settings->overall_timeout, 0);
		igt_assert(!settings->use_watchdog);
		igt_assert_eq(settings->prune_mode, 0);
		igt_assert(strstr(settings->test_root, "test-root-dir") != NULL);
		igt_assert(strstr(settings->results_path, "path-to-results") != NULL);

		igt_assert(!settings->piglit_style_dmesg);
		igt_assert_eq(settings->dmesg_warn_level, 4);
	}

	igt_subtest_group {
		char *cwd;
		char *path;

		igt_fixture {
			igt_require((cwd = realpath(".", NULL)) != NULL);
			path = NULL;
		}

		igt_subtest("absolute-path-converter") {
			char paths[][15] = { "simple-name", "foo/bar", "." };
			size_t i;

			for (i = 0; i < ARRAY_SIZE(paths); i++) {
				free(path);
				path = absolute_path(paths[i]);

				igt_assert(path[0] == '/');
				igt_debug("Got path %s for %s\n", path, paths[i]);
				igt_assert(strstr(path, cwd) == path);
				if (strcmp(paths[i], ".")) {
					igt_assert(strstr(path, paths[i]) != NULL);
				}
			}
		}

		igt_fixture {
			free(cwd);
			free(path);
		}
	}

	igt_subtest_group {
		const char tmptestlist[] = "tmp.testlist";
		char dirname[] = "tmpdirXXXXXX";
		char pathtotestlist[64];
		volatile char *path;

		igt_fixture {
			int dirfd, fd;

			path = NULL;

			igt_require(mkdtemp(dirname) != NULL);
			igt_require((dirfd = open(dirname, O_DIRECTORY | O_RDONLY)) >= 0);
			igt_require((fd = openat(dirfd, tmptestlist, O_CREAT | O_EXCL | O_WRONLY, 0660)) >= 0);
			close(fd);
			close(dirfd);

			strcpy(pathtotestlist, dirname);
			strcat(pathtotestlist, "/");
			strcat(pathtotestlist, tmptestlist);
		}

		igt_subtest("absolute-path-usage") {
			const char *argv[] = { "runner",
					       "--allow-non-root",
					       "--test-list", pathtotestlist,
					       testdatadir,
					       dirname,
			};

			igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));

			path = realpath(testdatadir, NULL);
			igt_assert(path != NULL);
			igt_assert_eqstr(settings->test_root, (char*)path);
			free((void*)path);
			path = realpath(dirname, NULL);
			igt_assert(path != NULL);
			igt_assert_eqstr(settings->results_path, (char*)path);
			free((void*)path);
			path = realpath(pathtotestlist, NULL);
			igt_assert(path != NULL);
			igt_assert_eqstr(settings->test_list, (char*)path);
		}

		igt_fixture {
			int dirfd;

			igt_require((dirfd = open(dirname, O_DIRECTORY | O_RDONLY)) >= 0);
			unlinkat(dirfd, tmptestlist, 0);
			close(dirfd);
			rmdir(dirname);

			free((void*)path);
		}
	}

	igt_subtest("environment-overrides-test-root-flag") {
		const char *argv[] = { "runner",
				       "--allow-non-root",
				       "test-root-dir",
				       "path-to-results",
		};

		setenv("IGT_TEST_ROOT", testdatadir, 1);
		igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));

		igt_assert_eq(settings->abort_mask, 0);
		igt_assert(!settings->test_list);
		igt_assert_eqstr(settings->name, "path-to-results");
		igt_assert(!settings->dry_run);
		igt_assert_eq(settings->include_regexes.size, 0);
		igt_assert_eq(settings->exclude_regexes.size, 0);
		igt_assert(!settings->sync);
		igt_assert_eq(settings->log_level, LOG_LEVEL_NORMAL);
		igt_assert(!settings->overwrite);
		igt_assert(!settings->multiple_mode);
		igt_assert_eq(settings->inactivity_timeout, 0);
		igt_assert_eq(settings->per_test_timeout, 0);
		igt_assert_eq(settings->overall_timeout, 0);
		igt_assert(!settings->use_watchdog);
		igt_assert(strstr(settings->test_root, testdatadir) != NULL);
		igt_assert(strstr(settings->results_path, "path-to-results") != NULL);
		igt_assert(!settings->piglit_style_dmesg);
	}

	igt_fixture {
		unsetenv("IGT_TEST_ROOT");
	}

	igt_subtest("parse-all-settings") {
		char blacklist_name[PATH_MAX], blacklist2_name[PATH_MAX];
		struct environment_variable *env_var;

		const char *argv[] = { "runner",
				       "--allow-non-root",
				       "-n", "foo",
				       "--abort-on-monitored-error=taint,lockdep",
				       "--disk-usage-limit=4096",
				       "--test-list", "path-to-test-list",
				       "--ignore-missing",
				       "--dry-run",
				       "-t", "pattern1",
				       "-t", "pattern2",
				       "-x", "xpattern1",
				       "-x", "xpattern2",
				       "-e", "HAVE_A_NICE=TESTING",
				       "--environment", "ENVS_WITH_JUST_KEYS",
				       "-b", blacklist_name,
				       "--blacklist", blacklist2_name,
				       "-s",
				       "-l", "verbose",
				       "--overwrite",
				       "--multiple-mode",
				       "--inactivity-timeout", "27",
				       "--per-test-timeout", "72",
				       "--overall-timeout", "360",
				       "--use-watchdog",
				       "--piglit-style-dmesg",
				       "--dmesg-warn-level=3",
				       "--collect-code-cov",
				       "--coverage-per-test",
				       "--collect-script", "/usr/bin/true",
				       "--prune-mode=keep-subtests",
				       "test-root-dir",
				       "path-to-results",
		};

		setenv("ENVS_WITH_JUST_KEYS", "SHOULD_WORK", 1);

		sprintf(blacklist_name, "%s/test-blacklist.txt", testdatadir);
		sprintf(blacklist2_name, "%s/test-blacklist2.txt", testdatadir);

		igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));

		igt_assert_eq(settings->abort_mask, ABORT_TAINT | ABORT_LOCKDEP);
		igt_assert_eq_u64(settings->disk_usage_limit, 4096UL);
		igt_assert(strstr(settings->test_list, "path-to-test-list") != NULL);
		igt_assert_eqstr(settings->name, "foo");
		igt_assert(settings->dry_run);
		igt_assert(settings->allow_non_root);

		igt_assert_eq(settings->include_regexes.size, 2);
		igt_assert_eqstr(settings->include_regexes.regex_strings[0], "pattern1");
		igt_assert_eqstr(settings->include_regexes.regex_strings[1], "pattern2");

		igt_assert_eq(settings->exclude_regexes.size, 4);
		igt_assert_eqstr(settings->exclude_regexes.regex_strings[0], "xpattern1");
		igt_assert_eqstr(settings->exclude_regexes.regex_strings[1], "xpattern2");
		igt_assert_eqstr(settings->exclude_regexes.regex_strings[2], "xpattern3"); /* From blacklist */
		igt_assert_eqstr(settings->exclude_regexes.regex_strings[3], "xpattern4"); /* From blacklist2 */

		igt_assert(!igt_list_empty(&settings->env_vars));

		env_var = igt_list_first_entry(&settings->env_vars, env_var, link);
		igt_assert_eqstr(env_var->key, "HAVE_A_NICE");
		igt_assert_eqstr(env_var->value, "TESTING");

		env_var = igt_list_last_entry(&settings->env_vars, env_var, link);
		igt_assert_eqstr(env_var->key, "ENVS_WITH_JUST_KEYS");
		igt_assert_eqstr(env_var->value, "SHOULD_WORK");

		igt_assert(settings->sync);
		igt_assert_eq(settings->log_level, LOG_LEVEL_VERBOSE);
		igt_assert(settings->overwrite);
		igt_assert(settings->multiple_mode);
		igt_assert_eq(settings->inactivity_timeout, 27);
		igt_assert_eq(settings->per_test_timeout, 72);
		igt_assert_eq(settings->overall_timeout, 360);
		igt_assert(settings->use_watchdog);
		igt_assert_eq(settings->prune_mode, PRUNE_KEEP_SUBTESTS);
		igt_assert(strstr(settings->test_root, "test-root-dir") != NULL);
		igt_assert(strstr(settings->results_path, "path-to-results") != NULL);

		igt_assert(settings->piglit_style_dmesg);
		igt_assert_eq(settings->dmesg_warn_level, 3);
	}
	igt_subtest("parse-list-all") {
		const char *argv[] = { "runner",
				       "--list-all",
				       "test-root-dir"};

		igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));
		igt_assert_eq(settings->list_all, 1);
	}

	igt_subtest("dmesg-warn-level-inferred") {
		const char *argv[] = { "runner",
				       "--allow-non-root",
				       "test-root-dir",
				       "path-to-results",
		};

		igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));

		igt_assert(!settings->piglit_style_dmesg);
		igt_assert_eq(settings->dmesg_warn_level, 4);
	}

	igt_subtest("dmesg-warn-level-inferred-with-piglit-style") {
		const char *argv[] = { "runner",
				       "--allow-non-root",
				       "--piglit-style-dmesg",
				       "test-root-dir",
				       "path-to-results",
		};

		igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));

		igt_assert(settings->piglit_style_dmesg);
		igt_assert_eq(settings->dmesg_warn_level, 5);
	}

	igt_subtest("dmesg-warn-level-overridable-with-piglit-style") {
		const char *argv[] = { "runner",
				       "--allow-non-root",
				       "--piglit-style-dmesg",
				       "--dmesg-warn-level=3",
				       "test-root-dir",
				       "path-to-results",
		};

		igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));

		igt_assert(settings->piglit_style_dmesg);
		igt_assert_eq(settings->dmesg_warn_level, 3);
	}

	igt_subtest("invalid-option") {
		const char *argv[] = { "runner",
				       "--no-such-option",
				       "test-root-dir",
				       "results-path",
		};

		igt_assert(!parse_options(ARRAY_SIZE(argv), (char**)argv, settings));
	}

	igt_subtest("paths-missing") {
		const char *argv[] = { "runner",
				       "-o",
		};
		igt_assert(!parse_options(ARRAY_SIZE(argv), (char**)argv, settings));
	}

	igt_subtest("log-levels") {
		const char *argv[] = { "runner",
				       "-l", "normal",
				       "test-root-dir",
				       "results-path",
		};

		igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));
		igt_assert_eq(settings->log_level, LOG_LEVEL_NORMAL);

		argv[2] = "quiet";
		igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));
		igt_assert_eq(settings->log_level, LOG_LEVEL_QUIET);

		argv[2] = "verbose";
		igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));
		igt_assert_eq(settings->log_level, LOG_LEVEL_VERBOSE);
	}

	igt_subtest("abort-conditions") {
		const char *argv[] = { "runner",
				       "--abort-on-monitored-error=taint",
				       "--allow-non-root",
				       "test-root-dir",
				       "results-path",
		};

		igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));
		igt_assert_eq(settings->abort_mask, ABORT_TAINT);

		argv[1] = "--abort-on-monitored-error=lockdep";
		igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));
		igt_assert_eq(settings->abort_mask, ABORT_LOCKDEP);

		argv[1] = "--abort-on-monitored-error=taint";
		igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));
		igt_assert_eq(settings->abort_mask, ABORT_TAINT);

		argv[1] = "--abort-on-monitored-error=lockdep,taint";
		igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));
		igt_assert_eq(settings->abort_mask, ABORT_TAINT | ABORT_LOCKDEP);

		argv[1] = "--abort-on-monitored-error=taint,lockdep";
		igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));
		igt_assert_eq(settings->abort_mask, ABORT_TAINT | ABORT_LOCKDEP);

		argv[1] = "--abort-on-monitored-error=all";
		igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));
		igt_assert_eq(settings->abort_mask, ABORT_ALL);

		argv[1] = "--abort-on-monitored-error=";
		igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));
		igt_assert_eq(settings->abort_mask, 0);

		argv[1] = "--abort-on-monitored-error=doesnotexist";
		igt_assert(!parse_options(ARRAY_SIZE(argv), (char**)argv, settings));

	}

	igt_subtest("disk-usage-limit-suffixes") {
		const char *argv[] = { "runner",
				       "--disk-usage-limit=4096",
				       "test-root-dir",
				       "results-path",
		};

		igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));
		igt_assert_eq_u64(settings->disk_usage_limit, 4096UL);

		argv[1] = "--disk-usage-limit=4k";
		igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));
		igt_assert_eq_u64(settings->disk_usage_limit, 4096UL);

		argv[1] = "--disk-usage-limit=1M";
		igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));
		igt_assert_eq_u64(settings->disk_usage_limit, 1024UL * 1024UL);

		argv[1] = "--disk-usage-limit=1G";
		igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));
		igt_assert_eq_u64(settings->disk_usage_limit, 1024UL * 1024UL * 1024UL);
	}

	igt_subtest("prune-modes") {
		const char *argv[] = { "runner",
			               "--prune-mode=keep-dynamic-subtests",
				       "test-root-dir",
				       "results-path",
		};

		igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));
		igt_assert_eq(settings->prune_mode, PRUNE_KEEP_DYNAMIC);

		argv[1] = "--prune-mode=keep-dynamic";
		igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));
		igt_assert_eq(settings->prune_mode, PRUNE_KEEP_DYNAMIC);

		argv[1] = "--prune-mode=keep-subtests";
		igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));
		igt_assert_eq(settings->prune_mode, PRUNE_KEEP_SUBTESTS);

		argv[1] = "--prune-mode=keep-all";
		igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));
		igt_assert_eq(settings->prune_mode, PRUNE_KEEP_ALL);

		argv[1] = "--prune-mode=keep-requested";
		igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));
		igt_assert_eq(settings->prune_mode, PRUNE_KEEP_REQUESTED);
	}

	igt_subtest("parse-clears-old-data") {
		const char *argv[] = { "runner",
				       "-n", "foo",
				       "--dry-run",
				       "--allow-non-root",
				       "test-root-dir",
				       "results-path",
		};

		igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));

		igt_assert_eqstr(settings->name, "foo");
		igt_assert(settings->dry_run);
		igt_assert(!settings->test_list);
		igt_assert(!settings->sync);

		argv[1] = "--test-list";
		argv[3] = "--sync";

		igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));

		igt_assert_eqstr(settings->name, "results-path");
		igt_assert(!settings->dry_run);
		igt_assert(strstr(settings->test_list, "foo") != NULL);
		igt_assert(settings->sync);
	}

	igt_subtest_group {
		char filename[] = "tmplistXXXXXX";

		igt_fixture {
			int fd;
			igt_require((fd = mkstemp(filename)) >= 0);
			close(fd);
		}

		igt_subtest("validate-ok") {
			const char *argv[] = { "runner",
					       "--allow-non-root",
					       "--test-list", filename,
					       testdatadir,
					       "path-to-results",
			};

			igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));

			igt_assert(validate_settings(settings));
		}

		igt_fixture {
			unlink(filename);
		}
	}

	igt_subtest("validate-no-test-list") {
		const char *nosuchfile = "no-such-file";
		const char *argv[] = { "runner",
				       "--test-list", nosuchfile,
				       testdatadir,
				       "path-to-results",
		};

		igt_assert_lt(open(nosuchfile, O_RDONLY), 0);
		igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));

		igt_assert(!validate_settings(settings));
	}

	igt_subtest_group {
		char dirname[] = "tmpdirXXXXXX";
		struct job_list *list = malloc(sizeof(*list));

		igt_fixture {
			igt_require(mkdtemp(dirname) != NULL);
			init_job_list(list);
		}

		igt_subtest("job-list-no-test-list-txt") {
			const char *argv[] = { "runner",
					       dirname,
					       "path-to-results",
			};

			igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));

			igt_assert(!create_job_list(list, settings));
		}

		igt_fixture {
			rmdir(dirname);
			free_job_list(list);
			free(list);
		}
	}

	job_list_filter_test("nofilters", "-n", "placeholderargs", NUM_TESTDATA_SUBTESTS, NUM_TESTDATA_BINARIES);
	job_list_filter_test("binary-include", "-t", "successtest", 2, 1);
	job_list_filter_test("binary-exclude", "-x", "successtest", NUM_TESTDATA_SUBTESTS - 2, NUM_TESTDATA_BINARIES - 1);
	job_list_filter_test("subtest-include", "-t", "first-subtest", 1, 1);
	job_list_filter_test("subtest-exclude", "-x", "second-subtest", NUM_TESTDATA_SUBTESTS - 1, NUM_TESTDATA_BINARIES);
	job_list_filter_test("piglit-names", "-t", "igt@successtest", 2, 1);
	job_list_filter_test("piglit-names-subtest", "-t", "igt@successtest@first", 1, 1);

	igt_subtest_group {
		char filename[] = "tmplistXXXXXX";
		const char testlisttext[] = "igt@successtest@first-subtest\n"
			"igt@successtest@second-subtest\n"
			"igt@nosubtests\n";
		int multiple;
		struct job_list *list = malloc(sizeof(*list));

		igt_fixture {
			int fd;
			igt_require((fd = mkstemp(filename)) >= 0);
			igt_require(write(fd, testlisttext, strlen(testlisttext)) == strlen(testlisttext));
			close(fd);
			init_job_list(list);
		}

		for (multiple = 0; multiple < 2; multiple++) {
			igt_subtest_f("job-list-testlist-%s", multiple ? "multiple" : "normal") {
				const char *argv[] = { "runner",
						       "--test-list", filename,
						       multiple ? "--multiple-mode" : "--sync",
						       testdatadir,
						       "path-to-results",
				};

				igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));
				igt_assert(create_job_list(list, settings));

				igt_assert_eq(list->size, multiple ? 2 : 3);

				igt_assert_eqstr(list->entries[0].binary, "successtest");
				if (!multiple) igt_assert_eqstr(list->entries[1].binary, "successtest");
				igt_assert_eqstr(list->entries[multiple ? 1 : 2].binary, "nosubtests");

				igt_assert_eq(list->entries[0].subtest_count, multiple ? 2 : 1);
				igt_assert_eq(list->entries[1].subtest_count, multiple ? 0 : 1);
				if (!multiple) igt_assert_eq(list->entries[2].subtest_count, 0);

				igt_assert_eqstr(list->entries[0].subtests[0], "first-subtest");
				igt_assert_eqstr(list->entries[multiple ? 0 : 1].subtests[multiple ? 1 : 0], "second-subtest");
			}

			igt_subtest_f("job-list-testlist-filtered-%s", multiple ? "multiple" : "normal") {
				const char *argv[] = { "runner",
						       "--test-list", filename,
						       multiple ? "--multiple-mode" : "--sync",
						       "-t", "successtest",
						       "-x", "first",
						       testdatadir,
						       "path-to-results",
				};

				igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));
				igt_assert(create_job_list(list, settings));

				igt_assert_eq(list->size, 1);
				igt_assert_eqstr(list->entries[0].binary, "successtest");

				igt_assert_eq(list->entries[0].subtest_count, 1);
				igt_assert_eqstr(list->entries[0].subtests[0], "second-subtest");
			}
		}

		igt_fixture {
			unlink(filename);
			free_job_list(list);
			free(list);
		}
	}

	igt_subtest_group {
		char filename[] = "tmplistXXXXXX";
		const char testlisttext[] = "igt@dynamic@dynamic-subtest@passing\n"
			"igt@dynamic@dynamic-subtest@failing\n"
			"igt@dynamic@different-subtest@passing\n";
		int multiple;
		struct job_list *list = malloc(sizeof(*list));

		igt_fixture {
			int fd;
			igt_require((fd = mkstemp(filename)) >= 0);
			igt_require(write(fd, testlisttext, strlen(testlisttext)) == strlen(testlisttext));
			close(fd);
			init_job_list(list);
		}

		for (multiple = 0; multiple < 2; multiple++) {
			igt_subtest_f("job-list-testlist-dynamic-%s", multiple ? "multiple" : "normal") {
				const char *argv[] = { "runner",
						       "--test-list", filename,
						       multiple ? "--multiple-mode" : "--sync",
						       testdatadir,
						       "path-to-results",
				};

				igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));
				igt_assert(create_job_list(list, settings));

				/*
				 * Normally we would combine different
				 * subtests of the same binary to the
				 * same execution when using
				 * multiple-mode. If dynamic subtests
				 * are used, no execution combining
				 * should occur.
				 */

				igt_assert_eq(list->size, 3);

				igt_assert_eqstr(list->entries[0].binary, "dynamic");
				igt_assert_eqstr(list->entries[1].binary, "dynamic");
				igt_assert_eqstr(list->entries[2].binary, "dynamic");

				igt_assert_eq(list->entries[0].subtest_count, 1);
				igt_assert_eq(list->entries[1].subtest_count, 1);
				igt_assert_eq(list->entries[2].subtest_count, 1);

				igt_assert_eqstr(list->entries[0].subtests[0], "dynamic-subtest@passing");
				igt_assert_eqstr(list->entries[1].subtests[0], "dynamic-subtest@failing");
				igt_assert_eqstr(list->entries[2].subtests[0], "different-subtest@passing");
			}
		}

		igt_fixture {
			unlink(filename);
			free_job_list(list);
			free(list);
		}
	}

	igt_subtest_group {
		char dirname[] = "tmpdirXXXXXX";
		volatile int dirfd = -1, fd = -1;
		struct settings *cmp_settings = malloc(sizeof(*cmp_settings));

		igt_fixture {
			igt_require(mkdtemp(dirname) != NULL);
			rmdir(dirname);
			init_settings(cmp_settings);
		}

		igt_subtest("settings-serialize") {
			const char *argv[] = { "runner",
					       "-n", "foo",
					       "--abort-on-monitored-error",
					       "--disk-usage-limit=4k",
					       "--test-list", "path-to-test-list",
					       "--ignore-missing",
					       "--dry-run",
					       "-t", "pattern1",
					       "-t", "pattern2",
					       "-x", "xpattern1",
					       "-x", "xpattern2",
					       "-s",
					       "-l", "verbose",
					       "--overwrite",
					       "--multiple-mode",
					       "--inactivity-timeout", "27",
					       "--per-test-timeout", "72",
					       "--overall-timeout", "360",
					       "--use-watchdog",
					       "--piglit-style-dmesg",
					       "--prune-mode=keep-all",
					       testdatadir,
					       dirname,
			};

			igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));

			igt_assert(serialize_settings(settings));

			dirfd = open(dirname, O_DIRECTORY, O_RDONLY);
			igt_assert_f(dirfd >= 0, "Serialization did not create the results directory\n");

			igt_assert_f((fd = openat(dirfd, "metadata.txt", O_RDONLY)),
				     "Opening %s/metadata.txt failed\n", dirname);
			close(fd);

			igt_assert_f(read_settings_from_dir(cmp_settings, dirfd), "Reading settings failed\n");
			assert_settings_equal(settings, cmp_settings);
		}

		igt_fixture {
			close(fd);
			close(dirfd);
			clear_directory(dirname);
			clear_settings(cmp_settings);
			free(cmp_settings);
		}
	}

	igt_subtest_group {
		char dirname[] = "tmpdirXXXXXX";
		volatile int dirfd = -1, fd = -1;
		struct job_list *list, *cmp_list;
		int multiple;

		list = malloc(sizeof(*list));
		cmp_list = malloc(sizeof(*cmp_list));

		igt_fixture {
			init_job_list(list);
			init_job_list(cmp_list);
			igt_require(mkdtemp(dirname) != NULL);
			rmdir(dirname);
		}

		for (multiple = 0; multiple < 2; multiple++) {
			igt_subtest_f("job-list-serialize-%s", multiple ? "multiple" : "normal") {
				const char *argv[] = { "runner",
						       /* Ugly */
						       multiple ? "--multiple-mode" : "--sync",
						       testdatadir,
						       dirname,
				};

				igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));
				igt_assert(create_job_list(list, settings));

				igt_assert(serialize_settings(settings));
				igt_assert(serialize_job_list(list, settings));

				dirfd = open(dirname, O_DIRECTORY, O_RDONLY);
				igt_assert_f(dirfd >= 0, "Serialization did not create the results directory\n");

				igt_assert_f((fd = openat(dirfd, "joblist.txt", O_RDONLY)) >= 0,
					     "Opening %s/joblist.txt failed\n", dirname);
				close(fd);
				fd = -1;

				igt_assert_f(read_job_list(cmp_list, dirfd), "Reading job list failed\n");
				assert_job_list_equal(list, cmp_list);
			}

			igt_fixture {
				close(fd);
				close(dirfd);
				clear_directory(dirname);
				free_job_list(cmp_list);
				free_job_list(list);
			}
		}

		igt_fixture {
			free(cmp_list);
			free(list);
		}
	}

	igt_subtest_group {
		char dirname[] = "tmpdirXXXXXX";
		struct job_list *list = malloc(sizeof(*list));
		volatile int dirfd = -1, subdirfd = -1, fd = -1;

		igt_fixture {
			init_job_list(list);
			igt_require(mkdtemp(dirname) != NULL);
			rmdir(dirname);
		}

		igt_subtest("dry-run-option") {
			struct execute_state state;
			struct environment_variable *env_var;

			const char *argv[] = { "runner",
					       "--dry-run",
					       "--allow-non-root",
					       "-e", "JUST_TESTING=ENV_VARS",
					       "-x", "^abort",
					       testdatadir,
					       dirname,
			};

			igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));
			igt_assert(create_job_list(list, settings));

			igt_assert(initialize_execute_state(&state, settings, list));
			igt_assert_eq(state.next, 0);
			igt_assert(state.dry);
			igt_assert_eq(list->size, NUM_TESTDATA_SUBTESTS - NUM_TESTDATA_ABORT_SUBTESTS);

			igt_assert_f((dirfd = open(dirname, O_DIRECTORY | O_RDONLY)) >= 0,
				     "Dry run initialization didn't create the results directory.\n");

			/* Execute from just initialize_execute_state should fail */
			igt_assert(execute(&state, settings, list));
			igt_assert_f(openat(dirfd, "0", O_DIRECTORY | O_RDONLY) < 0,
				     "Dry run executed when it should not have.\n");
			igt_assert_f((fd = openat(dirfd, "metadata.txt", O_RDONLY)) >= 0,
				     "Dry run initialization didn't serialize settings.\n");
			close(fd);
			igt_assert_f((fd = openat(dirfd, "joblist.txt", O_RDONLY)) >= 0,
				     "Dry run initialization didn't serialize the job list.\n");
			close(fd);
			igt_assert_f((fd = openat(dirfd, "environment.txt", O_RDONLY)) >= 0,
			             "Dry run initialization didn't serialize the environment file.\n");
			close(fd);
			igt_assert_f((fd = openat(dirfd, "uname.txt", O_RDONLY)) < 0,
				     "Dry run initialization created uname.txt.\n");

			igt_assert(initialize_execute_state_from_resume(dirfd, &state, settings, list));
			igt_assert_eq(state.next, 0);
			igt_assert(!state.dry);
			igt_assert_eq(list->size, NUM_TESTDATA_SUBTESTS - NUM_TESTDATA_ABORT_SUBTESTS);
			/* initialize_execute_state_from_resume() closes the dirfd */
			igt_assert_f((dirfd = open(dirname, O_DIRECTORY | O_RDONLY)) >= 0,
				     "Dry run resume somehow deleted the results directory.\n");

			/* Execute from resume should work */
			igt_assert(execute(&state, settings, list));
			igt_assert_f((fd = openat(dirfd, "uname.txt", O_RDONLY)) >= 0,
				     "Dry run resume didn't create uname.txt.\n");
			close(fd);
			igt_assert_f((subdirfd = openat(dirfd, "0", O_DIRECTORY | O_RDONLY)) >= 0,
				     "Dry run resume didn't create result directory.\n");
			igt_assert_f((fd = openat(subdirfd, "journal.txt", O_RDONLY)) >= 0,
				     "Dry run resume didn't create a journal.\n");

			env_var = igt_list_first_entry(&settings->env_vars, env_var, link);
			igt_assert_eqstr(env_var->key, "JUST_TESTING");
			igt_assert_eqstr(env_var->value, "ENV_VARS");
		}

		igt_fixture {
			close(fd);
			close(dirfd);
			close(subdirfd);
			clear_directory(dirname);
			free_job_list(list);
			free(list);
		}
	}

	igt_subtest_group {
		char dirname[] = "tmpdirXXXXXX";
		struct job_list *list = malloc(sizeof(*list));
		volatile int dirfd = -1, fd = -1;

		igt_fixture {
			init_job_list(list);
			igt_require(mkdtemp(dirname) != NULL);
			rmdir(dirname);
		}

		igt_subtest("execute-initialize-new-run") {
			struct execute_state state;
			const char *argv[] = { "runner",
					       "--allow-non-root",
					       testdatadir,
					       dirname,
			};

			igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));
			igt_assert(create_job_list(list, settings));

			igt_assert(initialize_execute_state(&state, settings, list));

			igt_assert_eq(state.next, 0);
			igt_assert_eq(list->size, NUM_TESTDATA_SUBTESTS);
			igt_assert_f((dirfd = open(dirname, O_DIRECTORY | O_RDONLY)) >= 0,
				     "Execute state initialization didn't create the results directory.\n");
			igt_assert_f((fd = openat(dirfd, "metadata.txt", O_RDONLY)) >= 0,
				     "Execute state initialization didn't serialize settings.\n");
			close(fd);
			igt_assert_f((fd = openat(dirfd, "joblist.txt", O_RDONLY)) >= 0,
				     "Execute state initialization didn't serialize the job list.\n");
			close(fd);
			igt_assert_f((fd = openat(dirfd, "journal.txt", O_RDONLY)) < 0,
				     "Execute state initialization created a journal.\n");
			igt_assert_f((fd = openat(dirfd, "uname.txt", O_RDONLY)) < 0,
				     "Execute state initialization created uname.txt.\n");
		}

		igt_fixture {
			close(fd);
			close(dirfd);
			clear_directory(dirname);
			free_job_list(list);
			free(list);
		}
	}

	igt_subtest_group {
		char dirname[] = "tmpdirXXXXXX";
		struct job_list *list = malloc(sizeof(*list));
		volatile int dirfd = -1, subdirfd = -1, fd = -1;

		igt_fixture {
			init_job_list(list);
			igt_require(mkdtemp(dirname) != NULL);
		}

		igt_subtest("execute-initialize-subtest-started") {
			struct execute_state state;
			const char *argv[] = { "runner",
					       "--allow-non-root",
					       "--multiple-mode",
					       "-t", "successtest",
					       testdatadir,
					       dirname,
			};
			const char journaltext[] = "first-subtest\n";
			const char excludestring[] = "!first-subtest";

			igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));
			igt_assert(create_job_list(list, settings));
			igt_assert(list->size == 1);
			igt_assert(list->entries[0].subtest_count == 0);

			igt_assert(serialize_settings(settings));
			igt_assert(serialize_job_list(list, settings));

			igt_assert((dirfd = open(dirname, O_DIRECTORY | O_RDONLY)) >= 0);
			igt_assert(mkdirat(dirfd, "0", 0770) == 0);
			igt_assert((subdirfd = openat(dirfd, "0", O_DIRECTORY | O_RDONLY)) >= 0);
			igt_assert((fd = openat(subdirfd, "journal.txt", O_CREAT | O_WRONLY | O_EXCL, 0660)) >= 0);
			igt_assert(write(fd, journaltext, strlen(journaltext)) == strlen(journaltext));

			free_job_list(list);
			clear_settings(settings);
			igt_assert(initialize_execute_state_from_resume(dirfd, &state, settings, list));

			igt_assert_eq(state.next, 0);
			igt_assert_eq(list->size, 1);
			igt_assert_eq(list->entries[0].subtest_count, 2);
			igt_assert_eqstr(list->entries[0].subtests[0], "*");
			igt_assert_eqstr(list->entries[0].subtests[1], excludestring);
		}

		igt_fixture {
			close(fd);
			close(subdirfd);
			close(dirfd);
			clear_directory(dirname);
			free_job_list(list);
			free(list);
		}
	}

	igt_subtest_group {
		char dirname[] = "tmpdirXXXXXX";
		struct job_list *list = malloc(sizeof(*list));
		volatile int dirfd = -1, subdirfd = -1, fd = -1;

		igt_fixture {
			init_job_list(list);
			igt_require(mkdtemp(dirname) != NULL);
		}

		igt_subtest("execute-initialize-subtest-started-comms") {
			struct execute_state state;
			const char *argv[] = { "runner",
					       "--allow-non-root",
					       "--multiple-mode",
					       "-t", "successtest",
					       testdatadir,
					       dirname,
			};
			const char excludestring[] = "!first-subtest";

			igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));
			igt_assert(create_job_list(list, settings));
			igt_assert(list->size == 1);
			igt_assert(list->entries[0].subtest_count == 0);

			igt_assert(serialize_settings(settings));
			igt_assert(serialize_job_list(list, settings));

			igt_assert((dirfd = open(dirname, O_DIRECTORY | O_RDONLY)) >= 0);
			igt_assert(mkdirat(dirfd, "0", 0770) == 0);
			igt_assert((subdirfd = openat(dirfd, "0", O_DIRECTORY | O_RDONLY)) >= 0);
			igt_assert((fd = openat(subdirfd, "comms", O_CREAT | O_WRONLY | O_EXCL, 0660)) >= 0);
			write_packet_with_canary(fd, runnerpacket_subtest_start("first-subtest"));

			free_job_list(list);
			clear_settings(settings);
			igt_assert(initialize_execute_state_from_resume(dirfd, &state, settings, list));

			igt_assert_eq(state.next, 0);
			igt_assert_eq(list->size, 1);
			igt_assert_eq(list->entries[0].subtest_count, 2);
			igt_assert_eqstr(list->entries[0].subtests[0], "*");
			igt_assert_eqstr(list->entries[0].subtests[1], excludestring);
		}

		igt_fixture {
			close(fd);
			close(subdirfd);
			close(dirfd);
			clear_directory(dirname);
			free_job_list(list);
			free(list);
		}
	}

	igt_subtest_group {
		char dirname[] = "tmpdirXXXXXX";
		struct job_list *list = malloc(sizeof(*list));
		volatile int dirfd = -1, subdirfd = -1, fd = -1;

		igt_fixture {
			init_job_list(list);
			igt_require(mkdtemp(dirname) != NULL);
		}

		igt_subtest("execute-initialize-all-subtests-started") {
			struct execute_state state;
			const char *argv[] = { "runner",
					       "--allow-non-root",
					       "--multiple-mode",
					       "-t", "successtest@first-subtest",
					       "-t", "successtest@second-subtest",
					       testdatadir,
					       dirname,
			};
			const char journaltext[] = "first-subtest\nsecond-subtest\n";

			igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));
			igt_assert(create_job_list(list, settings));
			igt_assert(list->size == 1);
			igt_assert(list->entries[0].subtest_count == 2);

			igt_assert(serialize_settings(settings));
			igt_assert(serialize_job_list(list, settings));

			igt_assert((dirfd = open(dirname, O_DIRECTORY | O_RDONLY)) >= 0);
			igt_assert(mkdirat(dirfd, "0", 0770) == 0);
			igt_assert((subdirfd = openat(dirfd, "0", O_DIRECTORY | O_RDONLY)) >= 0);
			igt_assert((fd = openat(subdirfd, "journal.txt", O_CREAT | O_WRONLY | O_EXCL, 0660)) >= 0);
			igt_assert(write(fd, journaltext, strlen(journaltext)) == strlen(journaltext));

			free_job_list(list);
			clear_settings(settings);
			igt_assert(initialize_execute_state_from_resume(dirfd, &state, settings, list));

			/* All subtests are in journal, the entry should be considered completed */
			igt_assert_eq(state.next, 1);
			igt_assert_eq(list->size, 1);
			igt_assert_eq(list->entries[0].subtest_count, 4);
		}

		igt_fixture {
			close(fd);
			close(subdirfd);
			close(dirfd);
			clear_directory(dirname);
			free_job_list(list);
			free(list);
		}
	}

	igt_subtest_group {
		char dirname[] = "tmpdirXXXXXX";
		struct job_list *list = malloc(sizeof(*list));
		volatile int dirfd = -1, subdirfd = -1, fd = -1;

		igt_fixture {
			init_job_list(list);
			igt_require(mkdtemp(dirname) != NULL);
		}

		igt_subtest("execute-initialize-all-subtests-started-comms") {
			struct execute_state state;
			const char *argv[] = { "runner",
					       "--allow-non-root",
					       "--multiple-mode",
					       "-t", "successtest@first-subtest",
					       "-t", "successtest@second-subtest",
					       testdatadir,
					       dirname,
			};

			igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));
			igt_assert(create_job_list(list, settings));
			igt_assert(list->size == 1);
			igt_assert(list->entries[0].subtest_count == 2);

			igt_assert(serialize_settings(settings));
			igt_assert(serialize_job_list(list, settings));

			igt_assert((dirfd = open(dirname, O_DIRECTORY | O_RDONLY)) >= 0);
			igt_assert(mkdirat(dirfd, "0", 0770) == 0);
			igt_assert((subdirfd = openat(dirfd, "0", O_DIRECTORY | O_RDONLY)) >= 0);
			igt_assert((fd = openat(subdirfd, "comms", O_CREAT | O_WRONLY | O_EXCL, 0660)) >= 0);
			write_packet_with_canary(fd, runnerpacket_subtest_start("first-subtest"));
			write_packet_with_canary(fd, runnerpacket_subtest_start("second-subtest"));

			free_job_list(list);
			clear_settings(settings);
			igt_assert(initialize_execute_state_from_resume(dirfd, &state, settings, list));

			/* All subtests are in journal, the entry should be considered completed */
			igt_assert_eq(state.next, 1);
			igt_assert_eq(list->size, 1);
			igt_assert_eq(list->entries[0].subtest_count, 4);
		}

		igt_fixture {
			close(fd);
			close(subdirfd);
			close(dirfd);
			clear_directory(dirname);
			free_job_list(list);
			free(list);
		}
	}

	igt_subtest_group {
		char dirname[] = "tmpdirXXXXXX";
		struct job_list *list = malloc(sizeof(*list));
		volatile int dirfd = -1, subdirfd = -1, fd = -1;

		igt_fixture {
			init_job_list(list);
			igt_require(mkdtemp(dirname) != NULL);
		}

		igt_subtest("execute-initialize-subtests-complete") {
			struct execute_state state;
			const char *argv[] = { "runner",
					       "--allow-non-root",
					       "--multiple-mode",
					       testdatadir,
					       dirname,
			};
			const char journaltext[] = "first-subtest\nsecond-subtest\nexit:0\n";

			igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));
			igt_assert(create_job_list(list, settings));
			igt_assert(list->size == NUM_TESTDATA_BINARIES);

			if (!strcmp(list->entries[0].binary, "no-subtests")) {
				struct job_list_entry tmp = list->entries[0];
				list->entries[0] = list->entries[1];
				list->entries[1] = tmp;
			}

			igt_assert(list->entries[0].subtest_count == 0);

			igt_assert(serialize_settings(settings));
			igt_assert(serialize_job_list(list, settings));

			igt_assert_lte(0, dirfd = open(dirname, O_DIRECTORY | O_RDONLY));
			igt_assert_eq(mkdirat(dirfd, "0", 0770), 0);
			igt_assert((subdirfd = openat(dirfd, "0", O_DIRECTORY | O_RDONLY)) >= 0);
			igt_assert_lte(0, fd = openat(subdirfd, "journal.txt", O_CREAT | O_WRONLY | O_EXCL, 0660));
			igt_assert_eq(write(fd, journaltext, sizeof(journaltext)), sizeof(journaltext));

			free_job_list(list);
			clear_settings(settings);
			igt_assert(initialize_execute_state_from_resume(dirfd, &state, settings, list));

			igt_assert_eq(state.next, 1);
			igt_assert_eq(list->size, NUM_TESTDATA_BINARIES);
		}

		igt_fixture {
			close(fd);
			close(subdirfd);
			close(dirfd);
			clear_directory(dirname);
			free_job_list(list);
			free(list);
		}
	}

	igt_subtest_group {
		char dirname[] = "tmpdirXXXXXX";
		struct job_list *list = malloc(sizeof(*list));
		volatile int dirfd = -1, subdirfd = -1, fd = -1;

		igt_fixture {
			init_job_list(list);
			igt_require(mkdtemp(dirname) != NULL);
		}

		igt_subtest("execute-initialize-subtests-complete-comms") {
			struct execute_state state;
			const char *argv[] = { "runner",
					       "--allow-non-root",
					       "--multiple-mode",
					       testdatadir,
					       dirname,
			};

			igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));
			igt_assert(create_job_list(list, settings));
			igt_assert(list->size == NUM_TESTDATA_BINARIES);

			if (!strcmp(list->entries[0].binary, "no-subtests")) {
				struct job_list_entry tmp = list->entries[0];
				list->entries[0] = list->entries[1];
				list->entries[1] = tmp;
			}

			igt_assert(list->entries[0].subtest_count == 0);

			igt_assert(serialize_settings(settings));
			igt_assert(serialize_job_list(list, settings));

			igt_assert_lte(0, dirfd = open(dirname, O_DIRECTORY | O_RDONLY));
			igt_assert_eq(mkdirat(dirfd, "0", 0770), 0);
			igt_assert((subdirfd = openat(dirfd, "0", O_DIRECTORY | O_RDONLY)) >= 0);
			igt_assert((fd = openat(subdirfd, "comms", O_CREAT | O_WRONLY | O_EXCL, 0660)) >= 0);
			write_packet_with_canary(fd, runnerpacket_subtest_start("first-subtest"));
			write_packet_with_canary(fd, runnerpacket_subtest_start("second-subtest"));
			write_packet_with_canary(fd, runnerpacket_exit(0, "0.000s"));

			free_job_list(list);
			clear_settings(settings);
			igt_assert(initialize_execute_state_from_resume(dirfd, &state, settings, list));

			igt_assert_eq(state.next, 1);
			igt_assert_eq(list->size, NUM_TESTDATA_BINARIES);
		}

		igt_fixture {
			close(fd);
			close(subdirfd);
			close(dirfd);
			clear_directory(dirname);
			free_job_list(list);
			free(list);
		}
	}

	igt_subtest_group {
		struct job_list *list = malloc(sizeof(*list));
		volatile int dirfd = -1, subdirfd = -1, fd = -1;
		int multiple;

		igt_fixture {
			init_job_list(list);
		}

		for (multiple = 0; multiple < 2; multiple++) {
			char dirname[] = "tmpdirXXXXXX";

			igt_fixture {
				igt_require(mkdtemp(dirname) != NULL);
				rmdir(dirname);
			}

			igt_subtest_f("execute-subtests-%s", multiple ? "multiple" : "normal") {
				struct execute_state state;
				const char *argv[] = { "runner",
						       "--allow-non-root",
						       multiple ? "--multiple-mode" : "--sync",
						       "-t", "successtest.*-subtest",
						       testdatadir,
						       dirname,
				};
				char testdirname[16];
				size_t expected_tests = multiple ? 1 : 2;
				size_t i;

				igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));
				igt_assert(create_job_list(list, settings));
				igt_assert(initialize_execute_state(&state, settings, list));

				igt_assert(execute(&state, settings, list));
				igt_assert_f((dirfd = open(dirname, O_DIRECTORY | O_RDONLY)) >= 0,
					     "Execute didn't create the results directory\n");

				igt_assert_f((fd = openat(dirfd, "uname.txt", O_RDONLY)) >= 0,
					     "Execute didn't create uname.txt\n");
				close(fd);
				fd = -1;

				for (i = 0; i < expected_tests; i++) {
					snprintf(testdirname, 16, "%zd", i);

					igt_assert_f((subdirfd = openat(dirfd, testdirname, O_DIRECTORY | O_RDONLY)) >= 0,
						     "Execute didn't create result directory '%s'\n", testdirname);
					assert_execution_results_exist(subdirfd);
					close(subdirfd);
				}

				snprintf(testdirname, 16, "%zd", expected_tests);
				igt_assert_f((subdirfd = openat(dirfd, testdirname, O_DIRECTORY | O_RDONLY)) < 0,
					     "Execute created too many directories\n");
			}

			igt_fixture {
				close(fd);
				close(subdirfd);
				close(dirfd);
				clear_directory(dirname);
				free_job_list(list);
			}
		}

		igt_fixture
			free(list);
	}

	igt_subtest_group {
		igt_subtest("metadata-read-old-style-infer-dmesg-warn-piglit-style") {
			char metadata[] = "piglit_style_dmesg : 1\n";
			FILE *f = fmemopen(metadata, strlen(metadata), "r");
			igt_assert(f);

			igt_assert(read_settings_from_file(settings, f));

			igt_assert(settings->piglit_style_dmesg);
			igt_assert_eq(settings->dmesg_warn_level, 5);

			fclose(f);
		}

		igt_subtest("metadata-read-overrides-dmesg-warn-piglit-style") {
			char metadata[] = "piglit_style_dmesg : 1\ndmesg_warn_level : 3";
			FILE *f = fmemopen(metadata, strlen(metadata), "r");
			igt_assert(f);

			igt_assert(read_settings_from_file(settings, f));

			igt_assert(settings->piglit_style_dmesg);
			igt_assert_eq(settings->dmesg_warn_level, 3);

			fclose(f);
		}

		igt_subtest("metadata-read-old-style-infer-dmesg-warn-default") {
			char metadata[] = "piglit_style_dmesg : 0\n";
			FILE *f = fmemopen(metadata, strlen(metadata), "r");
			igt_assert(f);

			igt_assert(read_settings_from_file(settings, f));

			igt_assert(!settings->piglit_style_dmesg);
			igt_assert_eq(settings->dmesg_warn_level, 4);

			fclose(f);
		}

		igt_subtest("metadata-read-overrides-dmesg-warn-default") {
			char metadata[] = "piglit_style_dmesg : 0\ndmesg_warn_level : 3";
			FILE *f = fmemopen(metadata, strlen(metadata), "r");
			igt_assert(f);

			igt_assert(read_settings_from_file(settings, f));

			igt_assert(!settings->piglit_style_dmesg);
			igt_assert_eq(settings->dmesg_warn_level, 3);

			fclose(f);
		}

		igt_subtest("metadata-read-spaces") {
			char metadata[] = "name : foo bar\n";
			FILE *f = fmemopen(metadata, strlen(metadata), "r");
			igt_assert(f);

			igt_assert(read_settings_from_file(settings, f));

			igt_assert_eqstr(settings->name, "foo bar");

			fclose(f);
		}
	}

	igt_subtest_group {
		struct job_list *list = malloc(sizeof(*list));
		volatile int dirfd = -1, subdirfd = -1, fd = -1;
		int multiple;

		igt_fixture {
			init_job_list(list);
		}

		for (multiple = 0; multiple < 2; multiple++) {
			char dirname[] = "tmpdirXXXXXX";

			igt_fixture {
				/* This test checks that the stdout parsing for result without time data works, so use that */
				setenv("IGT_RUNNER_DISABLE_SOCKET_COMMUNICATION", "1", 1);
				igt_require(mkdtemp(dirname) != NULL);
				rmdir(dirname);
			}

			igt_subtest_f("execute-skipper-journal-%s", multiple ? "multiple" : "normal") {
				struct execute_state state;
				const char *argv[] = { "runner",
						       "--allow-non-root",
						       multiple ? "--multiple-mode" : "--sync",
						       "-t", "skippers",
						       testdatadir,
						       dirname,
				};
				char *dump;
				const char *expected_0 = multiple ?
					"skip-one\nskip-two\nexit:77 (" :
					"skip-one\nexit:77 (";
				const char *expected_1 = "skip-two\nexit:77 (";

				igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));
				igt_assert(create_job_list(list, settings));
				igt_assert(initialize_execute_state(&state, settings, list));

				igt_assert(execute(&state, settings, list));
				igt_assert_f((dirfd = open(dirname, O_DIRECTORY | O_RDONLY)) >= 0,
					     "Execute didn't create the results directory\n");

				igt_assert_f((fd = openat(dirfd, "uname.txt", O_RDONLY)) >= 0,
					     "Execute didn't create uname.txt\n");
				close(fd);
				fd = -1;

				igt_assert_f((subdirfd = openat(dirfd, "0", O_DIRECTORY | O_RDONLY)) >= 0,
					     "Execute didn't create result directory '0'\n");
				dump = dump_file(subdirfd, "journal.txt");
				igt_assert_f(dump != NULL,
					     "Execute didn't create the journal\n");
				/* Trim out the runtime */
				dump[strlen(expected_0)] = '\0';
				igt_assert_eqstr(dump, expected_0);
				free(dump);
				close(subdirfd);
				subdirfd = -1;

				if (!multiple) {
					igt_assert_f((subdirfd = openat(dirfd, "1", O_DIRECTORY | O_RDONLY)) >= 0,
						     "Execute didn't create result directory '1'\n");
					dump = dump_file(subdirfd, "journal.txt");
					igt_assert_f(dump != NULL,
						     "Execute didn't create the journal\n");
					dump[strlen(expected_1)] = '\0';
					igt_assert_eqstr(dump, expected_1);
					free(dump);
					close(subdirfd);
					subdirfd = -1;
				}
			}

			igt_fixture {
				close(fd);
				close(subdirfd);
				close(dirfd);
				clear_directory(dirname);
				free_job_list(list);
				unsetenv("IGT_RUNNER_DISABLE_SOCKET_COMMUNICATION");
			}
		}

		igt_fixture
			free(list);
	}

	igt_subtest_group {
		const char testlisttext[] = "igt@dynamic@dynamic-subtest@passing\n";
		struct job_list *list = malloc(sizeof(*list));
		volatile int dirfd = -1;
		char dirname[] = "tmpdirXXXXXX";
		volatile int fd;
		char filename[] = "tmplistXXXXXX";

		igt_fixture {
			igt_require(mkdtemp(dirname) != NULL);
			rmdir(dirname);

			igt_require((fd = mkstemp(filename)) >= 0);
			igt_require(write(fd, testlisttext, strlen(testlisttext)) == strlen(testlisttext));
			close(fd);

			init_job_list(list);
		}

		igt_subtest("dynamic-subtests-in-testlist") {
			struct execute_state state;
			struct json_object *results, *tests;
			const char *argv[] = { "runner",
					       "--allow-non-root",
					       "--test-list", filename,
					       testdatadir,
					       dirname,
			};

			igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));

			igt_assert(create_job_list(list, settings));
			igt_assert_eq(list->size, 1);
			igt_assert_eq(list->entries[0].subtest_count, 1);

			igt_assert(initialize_execute_state(&state, settings, list));
			igt_assert(execute(&state, settings, list));

			igt_assert_f((dirfd = open(dirname, O_DIRECTORY | O_RDONLY)) >= 0,
				     "Execute didn't create the results directory\n");
			igt_assert_f((results = generate_results_json(dirfd)) != NULL,
				     "Results parsing failed\n");

			igt_assert(json_object_object_get_ex(results, "tests", &tests));

			/* Check that the dynamic subtest we didn't request is not reported */
			igt_assert_no_result_for(tests, "igt@dynamic@dynamic-subtest@failing");

			/* Check that the dynamic subtest we did request is */
			igt_assert_eqstr(igt_get_result(tests, "igt@dynamic@dynamic-subtest@passing"), "pass");

			igt_assert_eq(json_object_put(results), 1);
		}

		igt_fixture {
			unlink(filename);
			close(dirfd);
			clear_directory(dirname);
			free_job_list(list);
			free(list);
		}
	}

	igt_subtest_group {
		struct job_list *list = malloc(sizeof(*list));
		volatile int dirfd = -1;
		char dirname[] = "tmpdirXXXXXX";

		igt_fixture {
			igt_require(mkdtemp(dirname) != NULL);
			rmdir(dirname);

			init_job_list(list);
		}

		igt_subtest("dynamic-subtest-failure-should-not-cause-warn") {
			struct execute_state state;
			struct json_object *results, *tests;
			const char *argv[] = { "runner",
					       "--allow-non-root",
					       "-t", "^dynamic$",
					       testdatadir,
					       dirname,
			};

			igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));
			igt_assert(create_job_list(list, settings));
			igt_assert(initialize_execute_state(&state, settings, list));
			igt_assert(execute(&state, settings, list));

			igt_assert_f((dirfd = open(dirname, O_DIRECTORY | O_RDONLY)) >= 0,
				     "Execute didn't create the results directory\n");
			igt_assert_f((results = generate_results_json(dirfd)) != NULL,
				     "Results parsing failed\n");

			igt_assert(json_object_object_get_ex(results, "tests", &tests));

			igt_assert_eqstr(igt_get_result(tests, "igt@dynamic@dynamic-subtest@passing"), "pass");

			igt_assert_eq(json_object_put(results), 1);
		}

		igt_fixture {
			close(dirfd);
			clear_directory(dirname);
			free_job_list(list);
		}
	}

	igt_subtest_group {
		struct job_list *list = malloc(sizeof(*list));
		volatile int dirfd = -1;
		char dirname[] = "tmpdirXXXXXX";

		igt_fixture {
			igt_require(mkdtemp(dirname) != NULL);
			rmdir(dirname);

			init_job_list(list);
		}

		igt_subtest("execute-abort-simple") {
			struct execute_state state;
			struct json_object *results, *tests;
			const char *argv[] = { "runner",
					       "--allow-non-root",
					       "-t", "^abort-simple$",
					       testdatadir,
					       dirname,
			};

			igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));
			igt_assert(create_job_list(list, settings));
			igt_assert(initialize_execute_state(&state, settings, list));
			igt_assert(!execute(&state, settings, list)); /* false = signal abort */

			igt_assert_f((dirfd = open(dirname, O_DIRECTORY | O_RDONLY)) >= 0,
				     "Execute didn't create the results directory\n");
			igt_assert_f((results = generate_results_json(dirfd)) != NULL,
				     "Results parsing failed\n");

			igt_assert(json_object_object_get_ex(results, "tests", &tests));

			igt_assert_eqstr(igt_get_result(tests, "igt@abort-simple"), "abort");

			igt_assert_eq(json_object_put(results), 1);
		}

		igt_fixture {
			close(dirfd);
			clear_directory(dirname);
			free_job_list(list);
		}
	}

	igt_subtest_group {
		struct job_list *list = malloc(sizeof(*list));
		volatile int dirfd = -1;

		for (int multiple = 0; multiple <= 1; ++multiple) {
			char dirname[] = "tmpdirXXXXXX";

			igt_fixture {
				igt_require(mkdtemp(dirname) != NULL);
				rmdir(dirname);

				init_job_list(list);
			}

			igt_subtest_f("execute-abort%s", multiple ? "-multiple" : "") {
				struct execute_state state;
				struct json_object *results, *tests;
				const char *argv[] = { "runner",
						       "--allow-non-root",
						       "-t", "^abort$",
						       multiple ? "--multiple-mode" : "--sync",
						       testdatadir,
						       dirname,
				};

				igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));
				igt_assert(create_job_list(list, settings));
				igt_assert(initialize_execute_state(&state, settings, list));
				igt_assert(!execute(&state, settings, list)); /* false = signal abort */

				igt_assert_f((dirfd = open(dirname, O_DIRECTORY | O_RDONLY)) >= 0,
					     "Execute didn't create the results directory\n");
				igt_assert_f((results = generate_results_json(dirfd)) != NULL,
					     "Results parsing failed\n");

				igt_assert(json_object_object_get_ex(results, "tests", &tests));

				igt_assert_eqstr(igt_get_result(tests, "igt@abort@a-subtest"), "pass");
				igt_assert_eqstr(igt_get_result(tests, "igt@abort@b-subtest"), "abort");

				if (multiple) /* no notrun injection for multiple mode */
					igt_assert_no_result_for(tests, "igt@abort@c-subtest");
				else
					igt_assert_eqstr(igt_get_result(tests, "igt@abort@c-subtest"), "notrun");

				igt_assert_eq(json_object_put(results), 1);
			}

			igt_fixture {
				close(dirfd);
				clear_directory(dirname);
				free_job_list(list);
			}
		}
	}

	igt_subtest_group {
		struct job_list *list = malloc(sizeof(*list));
		volatile int dirfd = -1;

		for (int multiple = 0; multiple <= 1; ++multiple) {
			char dirname[] = "tmpdirXXXXXX";

			igt_fixture {
				igt_require(mkdtemp(dirname) != NULL);
				rmdir(dirname);

				init_job_list(list);
			}

			igt_subtest_f("execute-abort-fixture%s", multiple ? "-multiple" : "") {
				struct execute_state state;
				struct json_object *results, *tests;
				const char *argv[] = { "runner", multiple ? "--multiple-mode" : "--sync",
						       "--allow-non-root",
						       "-t", "^abort-fixture$",
						       testdatadir,
						       dirname,
				};

				igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));
				igt_assert(create_job_list(list, settings));
				igt_assert(initialize_execute_state(&state, settings, list));
				igt_assert(!execute(&state, settings, list)); /* false = signal abort */

				igt_assert_f((dirfd = open(dirname, O_DIRECTORY | O_RDONLY)) >= 0,
					     "Execute didn't create the results directory\n");
				igt_assert_f((results = generate_results_json(dirfd)) != NULL,
					     "Results parsing failed\n");

				igt_assert(json_object_object_get_ex(results, "tests", &tests));

				if (multiple) {
					/*
					 * running the whole binary via -t, no
					 * way of blaming the particular subtest
					 */
					igt_assert_eqstr(igt_get_result(tests, "igt@abort-fixture"), "abort");
					igt_assert_no_result_for(tests, "igt@abort-fixture@a-subtest");
					igt_assert_no_result_for(tests, "igt@abort-fixture@b-subtest");
				} else {
					igt_assert_eqstr(igt_get_result(tests, "igt@abort-fixture@a-subtest"), "abort");
					igt_assert_eqstr(igt_get_result(tests, "igt@abort-fixture@b-subtest"), "notrun");
				}

				igt_assert_eq(json_object_put(results), 1);
			}

			igt_fixture {
				close(dirfd);
				clear_directory(dirname);
				free_job_list(list);
			}
		}
	}

	igt_subtest_group {
		struct job_list *list = malloc(sizeof(*list));
		volatile int dirfd = -1;

		for (int multiple = 0; multiple <= 1; ++multiple) {
			char dirname[] = "tmpdirXXXXXX";
			char filename[] = "tmplistXXXXXX";
			const char testlisttext[] = "igt@abort-fixture@b-subtest\n"
				"igt@abort-fixture@a-subtest\n";

			igt_fixture {
				int fd;
				igt_require((fd = mkstemp(filename)) >= 0);
				igt_require(write(fd, testlisttext, strlen(testlisttext)) == strlen(testlisttext));
				close(fd);
				igt_require(mkdtemp(dirname) != NULL);
				rmdir(dirname);

				init_job_list(list);
			}

			igt_subtest_f("execute-abort-fixture-testlist%s", multiple ? "-multiple" : "") {
				struct execute_state state;
				struct json_object *results, *tests;
				const char *argv[] = { "runner", multiple ? "--multiple-mode" : "--sync",
						       "--allow-non-root",
						       "--test-list", filename,
						       testdatadir,
						       dirname,
				};

				igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));
				igt_assert(create_job_list(list, settings));
				igt_assert(initialize_execute_state(&state, settings, list));
				igt_assert(!execute(&state, settings, list)); /* false = signal abort */

				igt_assert_f((dirfd = open(dirname, O_DIRECTORY | O_RDONLY)) >= 0,
					     "Execute didn't create the results directory\n");
				igt_assert_f((results = generate_results_json(dirfd)) != NULL,
					     "Results parsing failed\n");

				igt_assert(json_object_object_get_ex(results, "tests", &tests));

				if (multiple) /* multiple mode = no notruns */
					igt_assert_no_result_for(tests, "igt@abort-fixture@a-subtest");
				else
					igt_assert_eqstr(igt_get_result(tests, "igt@abort-fixture@a-subtest"), "notrun");


				igt_assert_eqstr(igt_get_result(tests, "igt@abort-fixture@b-subtest"), "abort");

				igt_assert_eq(json_object_put(results), 1);
			}

			igt_fixture {
				unlink(filename);
				close(dirfd);
				clear_directory(dirname);
				free_job_list(list);
			}
		}
	}

	igt_subtest_group {
		struct job_list *list = malloc(sizeof(*list));
		volatile int dirfd = -1;

		for (int multiple = 0; multiple <= 1; ++multiple) {
			char dirname[] = "tmpdirXXXXXX";

			igt_fixture {
				igt_require(mkdtemp(dirname) != NULL);
				rmdir(dirname);

				init_job_list(list);
			}

			igt_subtest_f("execute-abort-dynamic%s", multiple ? "-multiple" : "") {
				struct execute_state state;
				struct json_object *results, *tests;
				const char *argv[] = { "runner", multiple ? "--multiple-mode" : "--sync",
						       "--allow-non-root",
						       "-t", "^abort-dynamic$",
						       testdatadir,
						       dirname,
				};

				igt_assert(parse_options(ARRAY_SIZE(argv), (char**)argv, settings));
				igt_assert(create_job_list(list, settings));
				igt_assert(initialize_execute_state(&state, settings, list));
				igt_assert(!execute(&state, settings, list)); /* false = signal abort */

				igt_assert_f((dirfd = open(dirname, O_DIRECTORY | O_RDONLY)) >= 0,
					     "Execute didn't create the results directory\n");
				igt_assert_f((results = generate_results_json(dirfd)) != NULL,
					     "Results parsing failed\n");

				igt_assert(json_object_object_get_ex(results, "tests", &tests));

				igt_assert_eqstr(igt_get_result(tests, "igt@abort-dynamic@a-subtest@dynamic-1"), "pass");
				igt_assert_eqstr(igt_get_result(tests, "igt@abort-dynamic@b-subtest@dynamic-1"), "pass");
				igt_assert_eqstr(igt_get_result(tests, "igt@abort-dynamic@b-subtest@dynamic-2"), "abort");

				igt_assert_no_result_for(tests, "igt@abort-dynamic@b-subtest@dynamic-3");

				if (multiple) /* multiple mode = no notruns */
					igt_assert_no_result_for(tests, "igt@abort-dynamic@c-subtest");
				else
					igt_assert_eqstr(igt_get_result(tests, "igt@abort-dynamic@c-subtest"), "notrun");

				igt_assert_eq(json_object_put(results), 1);
			}

			igt_fixture {
				close(dirfd);
				clear_directory(dirname);
				free_job_list(list);
			}
		}
	}

	igt_subtest("file-descriptor-leakage") {
		int i;

		/*
		 * This is a build-time test, and it's expected that
		 * all subtests are normally run. Keep this one at the
		 * end.
		 *
		 * Try to close some number of fds after stderr and
		 * expect EBADF for each one.
		 */
		for (i = 3; i < 400; i++) {
			errno = 0;
			igt_assert_neq(close(i), 0);
			igt_assert_eq(errno, EBADF);
		}
	}

	igt_fixture {
		clear_settings(settings);
		free(settings);
	}
}
