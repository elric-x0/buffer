#ifndef __BUFFER__
#define __BUFFER__

#include <stdint.h>
#include <stdlib.h>

typedef struct{
	uint8_t *base;
	size_t len;
	size_t size;	
}buffer_t;

int buffer_init(buffer_t *buff, size_t buffer_size);

void* buffer_read(buffer_t *buff, size_t offset, size_t size);
int buffer_write(buffer_t *buff, size_t offset, void *data, size_t size);

int buffer_empty(buffer_t *buff);
int buffer_delete(buffer_t *buff);

void* buffer_fread(buffer_t *buff, const char* filename);
void* buffer_fwrite(buffer_t *buff, const char* filename);
#endif
