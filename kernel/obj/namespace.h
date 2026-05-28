#ifndef OBJ_NAMESPACE_H
#define OBJ_NAMESPACE_H

#include <obj/object.h>
#include <obj/rights.h>

//register an object with a name in the global namespace, capping the rights callers can obtain
int ns_register(const char *name, object_t *obj, handle_rights_t max_rights);

//unregister an object by name
int ns_unregister(const char *name);

//lookup an object by name (returns with +1 ref, caller must deref)
//if max_rights_out is non-NULL the registered rights ceiling is written there
object_t *ns_lookup_ex(const char *name, handle_rights_t *max_rights_out);

//lookup an object by name (returns with +1 ref, caller must deref)
static inline object_t *ns_lookup(const char *name) {
    return ns_lookup_ex(name, NULL);
}

//list namespace entries (index is in/out for stateless iteration)
//returns number of entries filled 0 when done
int ns_list(void *entries, uint32 count, uint32 *index);

//initialize namespace
void ns_init(void);

#endif
