#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "flatpkg.h"
#define FPKG_MODULE "upgrade"
#include "fpkglog.h"

static bool ref_named(const char* ref, const char* const* refs, int nrefs) {
	char id[256];
	if (ostree_ref_id(ref, id, sizeof(id)) != 0) return false;
	for (int i = 0; i < nrefs; i++)
		if (strcmp(refs[i], id) == 0) return true;
	return false;
}

static void restrict_to_refs(flatpkg_changeset_t* cs,
			     const flatpkg_dep_array_t* world,
			     const char* const* refs, int nrefs) {
	assert(cs != NULL);
	assert(world != NULL);
	for (int r = 0; r < nrefs; r++) {
		bool known = false;
		for (size_t j = 0; j < world->count; j++)
			if (strcmp(world->items[j].name, refs[r]) == 0)
				known = true;
		if (!known)
			fpkg_warnf("%s is not in the world file; skipping",
				   refs[r]);
	}
	for (size_t i = 0; i < cs->count; i++) {
		flatpkg_change_t* c = &cs->items[i];
		if (c->type != FP_CHANGE_UPGRADE &&
		    c->type != FP_CHANGE_DOWNGRADE &&
		    c->type != FP_CHANGE_REINSTALL)
			continue;
		if (!ref_named(c->ref, refs, nrefs)) c->type = FP_CHANGE_KEEP;
	}
}

// Upgrade policy in the caller: solve the world, keep only the named refs
// when asked, show the table.  The prompt and the commit are the CLI's.
int ostree_plan_upgrade(unsigned flags, const char* const* refs,
			size_t nrefs, fpkg_plan_t* out) {
	assert(out != NULL);
	memset(out, 0, sizeof(*out));
	if (world_load(&out->world) != 0) return -1;
	if (solver_solve(&out->world, flags, &out->cs, NULL, 0) != 0) {
		fpkg_errorf("solver failed");
		goto fail;
	}
	if (nrefs > 0)
		restrict_to_refs(&out->cs, &out->world, refs, (int)nrefs);
	out->had_work = changeset_has_work(&out->cs);
	if (!out->had_work) {
		fpkg_infof("nothing to do");
		return 0;
	}
	changeset_show_full(&out->cs, NULL);
	return 0;
fail:
	changeset_free(&out->cs);
	world_free(&out->world);
	memset(out, 0, sizeof(*out));
	return -1;
}
