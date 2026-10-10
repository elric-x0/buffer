#ifndef __BUFFER__
#define __BUFFER__
 
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <stdbool.h>

#define BUFFER_MAX_SIZE ((size_t)16 * 1024 * 1024) //16mb

typedef enum{
	_OK,
	_EOF,
	_ERROR,
	_UNSET
}buffer_status_t;

typedef struct{
	uint8_t *base;/*backing storage*/
	size_t len;  /* bytes in use */
	size_t size; /* capacity in bytes*/	
}buffer_t;

typedef struct {
	buffer_t buffer;
	FILE *stream;
	size_t _offset;
	size_t chunk_size;
	buffer_status_t status;
}buffer_stream_t;

/*
 * @function: 
 * int buffer_init(buffer_t *buff, size_t buffer_size);
 * @Desc:
 *	Initialize buffer with a maximum size of 16mb
* @params:
*	- buffer_t *buff: buffer 
*	- size_t buffer_size: size of the buffer with a max of 16mb
* @return
*  - int : 
*		-1 on error
*		0 on success
*/
int buffer_init(buffer_t *buff, size_t buffer_size);

/*
 * @function: 
 * void* buffer_read(buffer_t *buff, size_t offset, size_t size);
 * @Desc:
 *	Read bytes from buffer starting from offset
* @params:
*	- buffer_t *buff: initialized buffer 
*	- size_t offset: offset from the buffer to start reading from
*	- size_t size: number of bytes to be read from the buffer 
* @return
*  - void * : 
*	- On success: a pointer to the beguining of the read bytes
*	- on error : NULL
*/
void* buffer_read(buffer_t *buff, size_t offset, size_t size);

/*
 * @function: 
 * int buffer_write(buffer_t *buff, size_t offset, const void *data, size_t size);
 * @Desc:
 *	Write bytes from buffer starting from offset
* @params:
*	- buffer_t *buff: initialized buffer 
*	- size_t offset: offset from the buffer to start reading from
*	- const void *data: pointer to data to bo written
*	- size_t size: number of bytes to be read from the buffer 
* @return
*  - void * : 
*	- On success: a pointer to the beguining of the read bytes
*	- on error : NULL
*/
int buffer_write(buffer_t *buff, size_t offset, const void *data, size_t size);

/*
* @function: 
* int buffer_empty(buffer_t *buff);
* @Desc:
*	empty a buffer 
* @params:
*	- buffer_t *buff: initialized buffer 
* @return
*  - int : 
*		-1 on error
*		0 on success
*/
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
*/
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


int buffer_stream_init(buffer_stream_t *buff_stream, size_t chunk_size, const char *filename);
int buffer_stream_next(buffer_stream_t *buff_stream);
int buffer_stream_prev(buffer_stream_t *buff_stream);
int buffer_stream_close(buffer_stream_t *buff_stream);
#endif
