#include "../include/buffer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int buffer_init(buffer_t *buff, size_t buff_size){
	if(!buff){
		fprintf(stderr, "Error:Invalid param\n");	
		return -1;
	}
	buff->base = malloc(buff_size * sizeof(uint8_t));
	if(!buff->base){
		fprintf(stderr, "Error:Failed memory allocation\n");	
		return -1;
	}
	buff->len = 0;
	buff->size = buff_size;
	return 0;
}

void* buffer_read(buffer_t *buff, size_t offset, size_t size){
	void *index = NULL;
	if(!buff){
		fprintf(stderr, "Error:Invalid param\n");	
		return index;
	}
	if(offset >= buff->size){
		fprintf(stdout, "Error:Out of range read \n");	
		return index;
	}
	return buff->base + offset;
}


int buffer_write(buffer_t *buff, size_t offset, void *data, size_t size){
	if(!buff || !data){
		fprintf(stderr, "Error:Invalid param\n");	
		return -1;
	}
	if((size+offset) > buff->size){
		fprintf(stdout, "Error:Out of range write \n");	
		return -1;
	}
	memcpy( buff->base+offset, data, size);
	return 0;
}

int buffer_empty(buffer_t *buff){
	if(!buff){
		fprintf(stderr, "Error:Invalid param\n");	
		return -1;
	}
	memset( buff->base, 0, buff->size);
	buff->len = 0;
	return 0;
}

int buffer_delete(buffer_t *buff){
	free(buff->base);
	free(buff);
	return 0;
}
