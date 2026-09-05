#ifndef FPKG_NOENV_H
#define FPKG_NOENV_H

// Pull the declarations in before poisoning, so the poison only bites code
// that is included afterwards (which is the point).
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>

#pragma GCC poison getenv secure_getenv getopt getopt_long getopt_long_only getsubopt environ

#endif