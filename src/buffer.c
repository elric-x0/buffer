#include "../include/buffer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int buffer_init(buffer_t *buff, size_t buff_size){
	if(!buff ){
		fprintf(stderr, "Error:Invalid param\n");	
		return -1;
	}
	buff->base = NULL;
	buff->len = 0;
	buff->size = 0;
	if(buff_size > BUFFER_MAX_SIZE || buff_size == 0){
		fprintf(stderr, "Error:Invalid buffer size, max: %zu\n", BUFFER_MAX_SIZE);	
		return -1;
	}
	buff->base = calloc(buff_size, sizeof(uint8_t));
	if(!buff->base){
		fprintf(stderr, "Error:Failed memory allocation\n");	
		buff->len = 0;
		buff->size = 0;
		return -1;
	}
	buff->len = 0;
	buff->size = buff_size;
	return 0;
}

void* buffer_read(buffer_t *buff, size_t offset, size_t size){
	void *index = NULL;
	if(!buff || !buff->base){
		fprintf(stderr, "Error:Invalid param\n");	
		return index;
	}
	if(offset > buff->len || size > buff->len - offset){
		fprintf(stderr, "Error:Out of range read\n");	
		return index;
	}
	return buff->base + offset;
}


int buffer_write(buffer_t *buff, size_t offset, const void *data, size_t size){
	if(!buff || !buff->base || !data){
		fprintf(stderr, "Error:Invalid param\n");	
		return -1;
	}
	if(offset > buff->size || size > buff->size - offset){
		fprintf(stderr, "Error:Out of range write\n");	
		return -1;
	}
	if(size == 0)
		return 0;

	memcpy(buff->base+offset, data, size);
	if((offset + size) > buff->len)
		buff->len = offset + size;
	return 0;
}

int buffer_empty(buffer_t *buff){
	if(!buff || !buff->base){
		fprintf(stderr, "Error:Invalid param\n");	
		return -1;
	}
	memset( buff->base, 0, buff->size);
	buff->len = 0;
	return 0;
}

int buffer_delete(buffer_t *buff, bool heap_allocated, void(*free_fn)(void *memory)){
	if(!buff)
		return -1;
	if(buff->base)
		free_fn ? free_fn(buff->base): free(buff->base);
	if(!heap_allocated){
		buff->base = NULL;
		buff->len = 0;
		buff->size = 0;
	}else
		free(buff);
	return 0;
}

int buffer_fread(buffer_t *buff, const char *filename, size_t size){
	if(!filename || !buff || !buff->base || size == 0 || size > buff->size){
		fprintf(stderr, "Error:Invalid param\n");	
		return -1;
	}
	FILE *fp = NULL;
	fp = fopen(filename, "rb");
	if(!fp){
		fprintf(stderr, "Error:Failed to open file - %s\n", filename);	
		return -1;
	}
	size_t read_count = fread(buff->base, sizeof(uint8_t), size, fp);	
	fclose(fp);
	if(read_count != size){
		buff->len = 0;
		memset(buff->base, 0, buff->size);
		fprintf(stderr, "Error:Failed to read file\n");	
		return -1;
	}
	buff->len = read_count;
	return 0;
}

int buffer_fwrite(buffer_t *buff, const char *filename){
	if(!buff || !buff->base || !filename){
		fprintf(stderr, "Error:Invalid param\n");	
		return -1;
	}
	FILE *fp = NULL;
	fp = fopen(filename, "wb");
	if(!fp){
		fprintf(stderr, "Error:Failed to open file - %s\n", filename);	
		return -1;
	}
	size_t write_count = fwrite(buff->base, sizeof(uint8_t), buff->len, fp);
	int close_rc = fclose(fp);
	if(write_count != buff->len || close_rc != 0){
		fprintf(stderr, "Error:Failed to write to file - %s\n", filename);	
		remove(filename);
		return -1;
	}
	return 0;
}
