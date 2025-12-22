#ifndef LIB_PATH_H
#define LIB_PATH_H

//normalize path in-place: collapse //, strip trailing /
//returns 0 on success -1 if path contains . or ..
int path_normalize(char *path);

#endif
