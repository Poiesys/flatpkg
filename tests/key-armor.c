// key-armor.c - a flatpakrepo's GPGKey, decoded.
//
// The value is base64, but what it encodes differs between repos: flathub
// encodes the binary key packets, NVIDIA encodes the ASCII-armored text.
// Armoring the decoded bytes unconditionally buried NVIDIA's key in a second
// armor and ostree refused it (GPGME: No data).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char* armor_gpg_key(const char* key);

static int failures;

static void check(const char* what, int ok) {
	if (ok) return;
	fprintf(stderr, "FAIL: %s\n", what);
	failures++;
}

static char* b64(const unsigned char* in, size_t n) {
	static const char a[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t o = 0;
	char* out = malloc(((n + 2) / 3) * 4 + 1);

	if (out == NULL) exit(2);
	for (size_t i = 0; i < n; i += 3) {
		unsigned v = (unsigned)in[i] << 16;

		if (i + 1 < n) v |= (unsigned)in[i + 1] << 8;
		if (i + 2 < n) v |= in[i + 2];
		out[o++] = a[(v >> 18) & 63];
		out[o++] = a[(v >> 12) & 63];
		out[o++] = i + 1 < n ? a[(v >> 6) & 63] : '=';
		out[o++] = i + 2 < n ? a[v & 63] : '=';
	}
	out[o] = '\0';
	return out;
}

int main(void) {
	// NVIDIA's shape: base64 of an armored key.  The answer is that armor,
	// byte for byte, not an armor of it.
	static const char armored[] =
		"-----BEGIN PGP PUBLIC KEY BLOCK-----\n"
		"\n"
		"mQGNBGe3DxcBDACa5Tu8NJeCDnKw3hb9vZITUPpFR5LVpvL+Xmz5\n"
		"-----END PGP PUBLIC KEY BLOCK-----\n";
	char* v = b64((const unsigned char*)armored, strlen(armored));
	char* got = armor_gpg_key(v);

	check("the armored key is not decoded", got != NULL && *got != '\0');
	if (got != NULL) {
		check("the decoded armor is written as it is",
		      strcmp(got, armored) == 0);
		check("the armor was not wrapped in a second armor",
		      strstr(got, "LS0t") == NULL);
	}
	free(got);
	free(v);

	// flathub's shape: base64 of the binary packets.  Those do need the
	// armor, and the CRC line is part of it.
	static const unsigned char packets[] = {0x99, 0x02, 0x0d, 0x04, 0x59,
						 0x43, 0xda, 0xc0, 0x01, 0x10};
	v = b64(packets, sizeof(packets));
	got = armor_gpg_key(v);
	check("the binary key is armored", got != NULL);
	if (got != NULL) {
		check("the armor header is there",
		      strncmp(got, "-----BEGIN PGP PUBLIC KEY BLOCK-----", 36) ==
			      0);
		check("the armor has a CRC line", strstr(got, "\n=") != NULL);
		check("the armor is closed",
		      strstr(got, "-----END PGP PUBLIC KEY BLOCK-----") != NULL);
		check("the packets are not the answer",
		      strstr(got, armored) == NULL);
	}
	free(got);
	free(v);

	// A repo file may hand the armor over undecoded; that passes through.
	got = armor_gpg_key(armored);
	check("an armored value passes through",
	      got != NULL && strcmp(got, armored) == 0);
	free(got);

	if (failures == 0)
		printf("PASS: a decoded armor stays an armor, and binary packets "
		       "are armored\n");
	return failures == 0 ? 0 : 1;
}
