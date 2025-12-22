#ifndef OBJ_NAMESPACE_H
#define OBJ_NAMESPACE_H

#include <obj/object.h>

//register an object with a name in the global namespace
int ns_register(const char *name, object_t *obj);

//unregister an object by name
int ns_unregister(const char *name);

//lookup an object by name (returns with +1 ref, caller must deref)
object_t *ns_lookup(const char *name);

//list namespace entries (index is in/out for stateless iteration)
//returns number of entries filled 0 when done
int ns_list(void *entries, uint32 count, uint32 *index);

//initialize namespace
void ns_init(void);

#endif
