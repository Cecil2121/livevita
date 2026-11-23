#ifndef IMGCACHE_H
#define IMGCACHE_H
/* LiveVita: pictures from the network (stream previews, category box art, chat emotes), kept in RAM as
 * textures with a small least-recently-used cache. A worker thread downloads; textures are made and freed
 * only on the UI thread (img_frame), after the GPU is done with them. */
#include <vita2d.h>

void img_start(void);                   /* once: starts the download thread */
/* Call at the start of every frame, before vita2d_start_drawing: turns finished downloads into textures (a
 * few per frame) and frees the least recently used ones beyond the limits. */
void img_frame(void);
/* The texture for url, or NULL while it is loading (or failed). Asks for it if needed. Marks it used. */
vita2d_texture *img_get(const char *url);
/* Forgets everything (frees all textures): call before leaving the app. */
void img_shutdown(void);

#endif
