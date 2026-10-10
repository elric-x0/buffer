#ifndef __BUFFER__
#define __BUFFER__

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define BUFFER_MAX_SIZE ((size_t)16 * 1024 * 1024) //16mb

typedef struct{
	uint8_t *base;/*backing storage*/
	size_t len;  /* bytes in use */
	size_t size; /* capacity in bytes*/	
}buffer_t;

int buffer_init(buffer_t *buff, size_t buffer_size);

void* buffer_read(buffer_t *buff, size_t offset, size_t size);
int buffer_write(buffer_t *buff, size_t offset, const void *data, size_t size);

int buffer_empty(buffer_t *buff);
int buffer_delete(buffer_t *buff, bool heap_allocated, void(*free_fn)(void *memory));

/*
 * @function: 
 * int buffer_fread(buffer_t *buff, const char* filename);
 * @Desc:
 * Loads the whole file, of fails if the file is too big(16mb)
 *		buff must be initialized with buffer_init();
 *		the caller owns it and frees it with buffer_delete
* @params:
*	- buffer_t *buffer: initialized buffer
*	- const char *filename: file to be read into the buffer
* @return
*  - int : 
*		-1 on error
*		0 on success
*  */
int buffer_fread(buffer_t *buff, const char* filename, size_t size);

/*
 * @function: 
 * int buffer_fwrite(buffer_t *buff, const char* filename);
 * @Desc:
 *	Write the content of the buffer in to a file
*
* @params:
*	- buffer_t *buffer: buffer holding the data
*	- const char *filename: file to save buffer into
* @return
*  - int : 
*		-1 on error
*		0 on success
*  */
int buffer_fwrite(buffer_t *buff, const char* filename);
#endif
