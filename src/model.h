#ifndef ZELL_MODEL_H
#define ZELL_MODEL_H

#include <stddef.h>

/* GPU and chat-model choice for the wizard. "auto" keeps the built-in
 * router and the Quadro-preferring device probe. */
#define ZELL_MODEL_SCHEMA "kilix.zell.model/v1"
#define ZELL_MODEL_AUTO "auto"

const char *zell_model_state_path(void);

/* 1 when the file was read, 0 when it is missing, -1 when it is unusable.
 * Both results leave device and model set; missing and unusable mean auto. */
int zell_model_parse(const char *text, char *device, size_t device_len,
                     char *model, size_t model_len);
int zell_model_load(char *device, size_t device_len,
                    char *model, size_t model_len);

/* 1 when the saved device is a specific Vulkan name. Auto and a missing
 * file return 0 and write nothing useful into out. */
int zell_model_device(char *out, size_t out_len);

/* Writes the saved model id, or auto when the file does not name one. */
void zell_model_id(char *out, size_t out_len);

#endif
