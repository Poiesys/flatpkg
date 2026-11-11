// portal-fc.c - drives the carved FileChooser option parser with real D-Bus
// messages.  A dict value is a variant, and get_basic() on the variant (or on
// any container) aborts the shared portal; a nested variant and a filter rule
// whose pattern field is not a string are both calls the parser has to
// survive.
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dbus/dbus.h>

char* xstrdup(const char* s);

#include "fc.inc"

static int bad;

static void note(const char* what) {
	printf("FAIL: %s\n", what);
	bad = 1;
}

static void add_bool(DBusMessageIter* arr, const char* key, dbus_bool_t v) {
	DBusMessageIter de, var;

	dbus_message_iter_open_container(arr, DBUS_TYPE_DICT_ENTRY, NULL, &de);
	dbus_message_iter_append_basic(&de, DBUS_TYPE_STRING, &key);
	dbus_message_iter_open_container(&de, DBUS_TYPE_VARIANT, "b", &var);
	dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &v);
	dbus_message_iter_close_container(&de, &var);
	dbus_message_iter_close_container(arr, &de);
}

static void add_string(DBusMessageIter* arr, const char* key, const char* v) {
	DBusMessageIter de, var;

	dbus_message_iter_open_container(arr, DBUS_TYPE_DICT_ENTRY, NULL, &de);
	dbus_message_iter_append_basic(&de, DBUS_TYPE_STRING, &key);
	dbus_message_iter_open_container(&de, DBUS_TYPE_VARIANT, "s", &var);
	dbus_message_iter_append_basic(&var, DBUS_TYPE_STRING, &v);
	dbus_message_iter_close_container(&de, &var);
	dbus_message_iter_close_container(arr, &de);
}

// A variant holding a variant: valid on the wire, not a boolean.
static void add_nested_bool(DBusMessageIter* arr, const char* key,
			    dbus_bool_t v) {
	DBusMessageIter de, var, inner;

	dbus_message_iter_open_container(arr, DBUS_TYPE_DICT_ENTRY, NULL, &de);
	dbus_message_iter_append_basic(&de, DBUS_TYPE_STRING, &key);
	dbus_message_iter_open_container(&de, DBUS_TYPE_VARIANT, "v", &var);
	dbus_message_iter_open_container(&var, DBUS_TYPE_VARIANT, "b", &inner);
	dbus_message_iter_append_basic(&inner, DBUS_TYPE_BOOLEAN, &v);
	dbus_message_iter_close_container(&var, &inner);
	dbus_message_iter_close_container(&de, &var);
	dbus_message_iter_close_container(arr, &de);
}

static void add_rule(DBusMessageIter* rules, const char* type,
		     const char* pattern) {
	DBusMessageIter st;

	dbus_message_iter_open_container(rules, DBUS_TYPE_STRUCT, NULL, &st);
	dbus_message_iter_append_basic(&st, DBUS_TYPE_STRING, &type);
	dbus_message_iter_append_basic(&st, DBUS_TYPE_STRING, &pattern);
	dbus_message_iter_close_container(rules, &st);
}

// a(sa(ss)): one filter name and two glob patterns.
static void add_filters(DBusMessageIter* arr, const char* key,
			const char* name) {
	DBusMessageIter de, var, fa, st, rules;

	dbus_message_iter_open_container(arr, DBUS_TYPE_DICT_ENTRY, NULL, &de);
	dbus_message_iter_append_basic(&de, DBUS_TYPE_STRING, &key);
	dbus_message_iter_open_container(&de, DBUS_TYPE_VARIANT, "a(sa(ss))",
					 &var);
	dbus_message_iter_open_container(&var, DBUS_TYPE_ARRAY, "(sa(ss))",
					 &fa);
	dbus_message_iter_open_container(&fa, DBUS_TYPE_STRUCT, NULL, &st);
	dbus_message_iter_append_basic(&st, DBUS_TYPE_STRING, &name);
	dbus_message_iter_open_container(&st, DBUS_TYPE_ARRAY, "(ss)", &rules);
	add_rule(&rules, "glob", "*.png");
	add_rule(&rules, "glob", "*.jpg");
	dbus_message_iter_close_container(&st, &rules);
	dbus_message_iter_close_container(&fa, &st);
	dbus_message_iter_close_container(&var, &fa);
	dbus_message_iter_close_container(&de, &var);
	dbus_message_iter_close_container(arr, &de);
}

// The same shape, but the rule's second field is a number: get_basic() into a
// string pointer used to be a str of it.
static void add_bad_filters(DBusMessageIter* arr, const char* key,
			    const char* name) {
	DBusMessageIter de, var, fa, st, rules, rule;
	dbus_uint32_t n = 7;
	const char* type = "glob";

	dbus_message_iter_open_container(arr, DBUS_TYPE_DICT_ENTRY, NULL, &de);
	dbus_message_iter_append_basic(&de, DBUS_TYPE_STRING, &key);
	dbus_message_iter_open_container(&de, DBUS_TYPE_VARIANT, "a(sa(su))",
					 &var);
	dbus_message_iter_open_container(&var, DBUS_TYPE_ARRAY, "(sa(su))",
					 &fa);
	dbus_message_iter_open_container(&fa, DBUS_TYPE_STRUCT, NULL, &st);
	dbus_message_iter_append_basic(&st, DBUS_TYPE_STRING, &name);
	dbus_message_iter_open_container(&st, DBUS_TYPE_ARRAY, "(su)", &rules);
	dbus_message_iter_open_container(&rules, DBUS_TYPE_STRUCT, NULL, &rule);
	dbus_message_iter_append_basic(&rule, DBUS_TYPE_STRING, &type);
	dbus_message_iter_append_basic(&rule, DBUS_TYPE_UINT32, &n);
	dbus_message_iter_close_container(&rules, &rule);
	dbus_message_iter_close_container(&st, &rules);
	dbus_message_iter_close_container(&fa, &st);
	dbus_message_iter_close_container(&var, &fa);
	dbus_message_iter_close_container(&de, &var);
	dbus_message_iter_close_container(arr, &de);
}

static void run(DBusMessage* m, bool want_multiple, bool want_multiple_set,
		const char* want_folder, const char* want_filter) {
	DBusMessageIter it;
	char* title = NULL;
	dbus_bool_t multiple = FALSE, dir_select = TRUE;
	char* folder = NULL;
	char** filters = NULL;
	size_t nfilters = 0;

	assert(dbus_message_iter_init(m, &it));
	if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY) {
		note("the test message does not start with the options array");
		return;
	}
	fc_parse_options(&it, &title, &multiple, &dir_select, &folder,
			 &filters, &nfilters);
	if (want_multiple_set && multiple != want_multiple)
		note("a boolean variant was not read");
	if (!want_multiple_set && multiple != FALSE)
		note("a nested variant was read as a boolean");
	if (dir_select != FALSE)
		note("a nested variant was read as a boolean");
	if (want_folder != NULL &&
	    (folder == NULL || strcmp(folder, want_folder) != 0))
		note("current_folder was not read");
	if (want_filter != NULL) {
		if (nfilters != 1)
			note("the filter list did not parse as one entry");
		else if (strcmp(filters[0], want_filter) != 0) {
			printf("FAIL: the filter reads '%s', wanted '%s'\n",
			       filters[0], want_filter);
			bad = 1;
		}
	}
	for (size_t i = 0; i < nfilters; i++) free(filters[i]);
	free(filters);
	free(folder);
	free(title);
}

// Spawn's cwd and argv arrive as ay, and get_basic() writes as the element's
// own type: an as element would put a pointer where the one-byte object is.
static void check_byte_arrays(void) {
	const unsigned char bytes[3] = {'a', 'b', 'c'};
	const char* str = "pointer";
	dbus_uint32_t num = 7;
	DBusMessage* m;
	DBusMessageIter it, arr;
	char out[16];

	m = dbus_message_new_method_call("a.b", "/a", "a.b", "X");
	assert(m != NULL);
	dbus_message_iter_init_append(m, &it);
	dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "y", &arr);
	for (size_t i = 0; i < sizeof(bytes); i++)
		dbus_message_iter_append_basic(&arr, DBUS_TYPE_BYTE, &bytes[i]);
	dbus_message_iter_close_container(&it, &arr);
	assert(dbus_message_iter_init(m, &it));
	if (iter_bytes_to_string(&it, out, sizeof(out)) != 0 ||
	    strcmp(out, "abc") != 0)
		note("an array of bytes was not read");
	if (iter_bytes_to_string(&it, out, 3) == 0)
		note("a byte array longer than the buffer was accepted");
	dbus_message_unref(m);

	m = dbus_message_new_method_call("a.b", "/a", "a.b", "X");
	assert(m != NULL);
	dbus_message_iter_init_append(m, &it);
	dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "s", &arr);
	dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING, &str);
	dbus_message_iter_close_container(&it, &arr);
	assert(dbus_message_iter_init(m, &it));
	memset(out, 0, sizeof(out));
	if (iter_bytes_to_string(&it, out, sizeof(out)) == 0)
		note("a string array element was read as a byte");
	dbus_message_unref(m);

	m = dbus_message_new_method_call("a.b", "/a", "a.b", "X");
	assert(m != NULL);
	dbus_message_iter_init_append(m, &it);
	dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "u", &arr);
	dbus_message_iter_append_basic(&arr, DBUS_TYPE_UINT32, &num);
	dbus_message_iter_close_container(&it, &arr);
	assert(dbus_message_iter_init(m, &it));
	memset(out, 0, sizeof(out));
	if (iter_bytes_to_string(&it, out, sizeof(out)) == 0)
		note("a uint32 array element was read as a byte");
	dbus_message_unref(m);
}

int main(void) {
	DBusMessage* m = dbus_message_new_method_call("a.b", "/a", "a.b", "X");
	DBusMessage* m2 = dbus_message_new_method_call("a.b", "/a", "a.b", "X");
	DBusMessageIter it, arr;

	assert(m != NULL);
	assert(m2 != NULL);
	dbus_message_iter_init_append(m, &it);
	dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &arr);
	add_bool(&arr, "multiple", TRUE);
	add_string(&arr, "current_folder", "/tmp/shots");
	add_filters(&arr, "filters", "Images");
	add_nested_bool(&arr, "directory_select", TRUE);
	dbus_message_iter_close_container(&it, &arr);
	run(m, TRUE, true, "/tmp/shots", "Images | *.png *.jpg");

	dbus_message_iter_init_append(m2, &it);
	dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &arr);
	add_bad_filters(&arr, "filters", "Numbers");
	dbus_message_iter_close_container(&it, &arr);
	run(m2, FALSE, false, NULL, "Numbers | *");

	dbus_message_unref(m);
	dbus_message_unref(m2);
	check_byte_arrays();
	if (bad) return 1;
	printf("ok   a variant's value is read, a container is not, and only a\n"
	       "     byte array becomes a byte string\n");
	return 0;
}
