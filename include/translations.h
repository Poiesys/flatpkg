#ifndef FPKG_TRANSLATIONS_H
#define FPKG_TRANSLATIONS_H

struct tr_entry {
	const char* msgid;
	const char* text;
};

const char* get_string(const char* msgid);

// Called by cli.c once the locale has been decided from the environment.
void translations_set_locale(const char* locale);

#endif