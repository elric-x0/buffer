#include "../include/buffer.h"
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

int buffer_stream_init(buffer_stream_t *buff_stream, size_t chunk_size, const char *filename){
	if(!buff_stream){
		fprintf(stderr, "Error:Invalid param\n");	
		return -1;
	}

	//set default values
	memset(buff_stream, 0, sizeof(buffer_stream_t));
	if(!filename || chunk_size == 0){
		fprintf(stderr, "Error:Invalid param\n");	
		return -1;
	}
	buff_stream->stream = fopen(filename, "rb");
	if(!buff_stream->stream){
		fprintf(stderr, "Error:Failed to open file - %s\n", filename);	
		return -1;
	}
	if(buffer_init(&buff_stream->buffer, chunk_size) != 0){
		fprintf(stderr, "Error:Failed to initialize buffer stream  - %s\n", filename);	
		fclose(buff_stream->stream);
		buff_stream->stream = NULL;
		return -1;
	}
	buff_stream->chunk_size = chunk_size;
	return 0;
}

static long read_stream(buffer_t *buff, size_t chunk_size, FILE *stream){
	if(chunk_size > buff->size){
		fprintf(stderr, "Error:Invalid params\n");	
		return -1;
	}
	size_t read_count = fread(buff->base, sizeof(uint8_t), chunk_size, stream);
	if(ferror(stream)){
		fprintf(stderr, "Error:Failed reading stream\n");	
		buffer_empty(buff);
		return -1;
	}
	if(read_count < chunk_size)
		memset(buff->base+read_count, 0, (buff->size - read_count));
	buff->len = read_count;
	return (long)read_count;
}

int buffer_stream_next(buffer_stream_t *buff_stream){
	if(!buff_stream || !buff_stream->stream || !buff_stream->buffer.base || (buff_stream->buffer.size != buff_stream->chunk_size)){
		fprintf(stderr, "Error:Invalid params\n");	
		return -1;
	}
	long read_rc = read_stream(&buff_stream->buffer, buff_stream->chunk_size, buff_stream->stream);
	if(read_rc >= 0)
		buff_stream->_offset+=(size_t)read_rc;
	return (int)read_rc;
}

int buffer_stream_prev(buffer_stream_t *buff_stream){
	if(!buff_stream || !buff_stream->stream || !buff_stream->buffer.base || buff_stream->buffer.size != buff_stream->chunk_size){
		fprintf(stderr, "Error:Invalid params\n");
		return -1;
	}
	size_t target_offset = 0;
	if(buff_stream->_offset >= buff_stream->chunk_size)
		target_offset = buff_stream->_offset - buff_stream->chunk_size;
	int seek_rc = fseek(buff_stream->stream, target_offset,SEEK_SET);
	if(seek_rc != 0){
		fprintf(stderr, "Error:Failed to position cursor on stream\n");	
		buffer_empty(&buff_stream->buffer);
		return -1;
	}
	buff_stream->_offset = target_offset; 
	long read_rc = read_stream(&buff_stream->buffer, buff_stream->chunk_size, buff_stream->stream);
	if(read_rc >= 0)
		buff_stream->_offset+=(size_t)read_rc;
	return (int)read_rc;
}

int buffer_stream_close(buffer_stream_t *buff_stream){
	if(!buff_stream){
		fprintf(stderr, "Error:Invalid params\n");	
		return -1;
	}
	if(buff_stream->stream && (fclose(buff_stream->stream) != 0)){
			fprintf(stderr, "Error:Failed to close stream\n");	
			return -1;
	}
	buffer_delete(&buff_stream->buffer, false, NULL);
	buff_stream->stream = NULL;
	buff_stream->_offset = 0;
	buff_stream->chunk_size = 0;
	return 0;
}
