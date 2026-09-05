#define FPKG_MODULE "translations"
#include "fpkglog.h"
#include <string.h>

#include "translations.h"
#include "translations/en.h"

static const struct tr_locale {
	const char* name;
	const struct tr_entry* entries;
	size_t n;
} tr_locales[] = {
	{"en", tr_en, sizeof(tr_en) / sizeof(tr_en[0])},
};

static const char* tr_current = "en";

void translations_set_locale(const char* locale) {
	char lang[16];
	size_t i;
	if (locale == NULL || *locale == '\0') {
		tr_current = "en";
		return;
	}
	i = 0;
	while (locale[i] != '\0' && locale[i] != '_' && locale[i] != '.' &&
	       locale[i] != '-' && i < sizeof(lang) - 1) {
		lang[i] = locale[i];
		i++;
	}
	lang[i] = '\0';
	for (size_t k = 0; k < sizeof(tr_locales) / sizeof(tr_locales[0]);
	     k++) {
		if (strcmp(tr_locales[k].name, lang) == 0) {
			tr_current = tr_locales[k].name;
			return;
		}
	}
	tr_current = "en";
}

static const struct tr_entry* tr_table(const char* name) {
	for (size_t k = 0; k < sizeof(tr_locales) / sizeof(tr_locales[0]); k++)
		if (strcmp(tr_locales[k].name, name) == 0)
			return tr_locales[k].entries;
	return tr_en;
}

static size_t tr_count(const char* name) {
	for (size_t k = 0; k < sizeof(tr_locales) / sizeof(tr_locales[0]); k++)
		if (strcmp(tr_locales[k].name, name) == 0)
			return tr_locales[k].n;
	return sizeof(tr_en) / sizeof(tr_en[0]);
}

const char* get_string(const char* msgid) {
	if (msgid == NULL) return NULL;

	for (size_t k = 0; k < tr_count(tr_current); k++) {
		const struct tr_entry* e = &tr_table(tr_current)[k];
		if (e->msgid != NULL && strcmp(e->msgid, msgid) == 0)
			return e->text != NULL ? e->text : msgid;
	}
	if (strcmp(tr_current, "en") != 0) {
		for (size_t k = 0; k < tr_count("en"); k++) {
			const struct tr_entry* e = &tr_table("en")[k];
			if (e->msgid != NULL && strcmp(e->msgid, msgid) == 0)
				return e->text != NULL ? e->text : msgid;
		}
	}
	return msgid;
}