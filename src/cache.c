/*
 * cache.c - A generic cache module
 *
 * Copyright (c) 2014   A. Dilly
 *
 * AirCat is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation.
 *
 * AirCat is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with AirCat.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "cache.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#define BUFFER_SIZE 8192

struct cache_format {
	struct a_format fmt;
	unsigned long len;
	struct cache_format *next;
};

struct cache_handle {
	/* Cache properties */
	int use_thread;
	/* Input callback */
	a_read_cb input_callback;
	void *user_data;
	/* Buffer handling */
	unsigned char *buffer;
	unsigned long size;
	unsigned long len;
	unsigned long pos;
	int is_ready;
	/* Associated format to buffer */
	struct cache_format *fmt_first;
	struct cache_format *fmt_last;
	unsigned long fmt_len;
	/* Thread objects */
	pthread_t thread;
	pthread_mutex_t mutex;
	pthread_mutex_t input_lock;
	int flush;
	int stop;
};

static void *cache_read_thread(void *user_data);

int cache_open(struct cache_handle **handle, unsigned long size, int use_thread,
	       a_read_cb input_callback, void *user_data)
{
	struct cache_handle *h;

	if(size == 0 || input_callback == NULL)
		return -1;

	/* Alloc structure */
	*handle = malloc(sizeof(struct cache_handle));
	if(*handle == NULL)
		return -1;
	h = *handle;

	/* Init structure */
	h->buffer = NULL;
	h->size = size;
	h->len = 0;
	h->pos = 0;
	h->is_ready = 0;
	h->input_callback = input_callback;
	h->user_data = user_data;
	h->use_thread = use_thread;
	h->stop = 0;
	h->fmt_first = NULL;
	h->fmt_last = NULL;
	h->fmt_len = 0;

	/* Allocate buffer */
	h->buffer = malloc(size * 4);
	if(h->buffer == NULL)
		return -1;

	/* Init thread mutex */
	pthread_mutex_init(&h->mutex, NULL);
	pthread_mutex_init(&h->input_lock, NULL);

	if(use_thread)
	{
		/* Create thread */
		if(pthread_create(&h->thread, NULL, cache_read_thread, h) != 0)
			return -1;
	}

	return 0;
}

int cache_is_ready(struct cache_handle *h)
{
	int ret;

	/* Lock cache access */
	pthread_mutex_lock(&h->mutex);

	/* Check data availability in cache */
	ret = h->is_ready;

	/* Unlock cache access */
	pthread_mutex_unlock(&h->mutex);

	return ret;
}

unsigned char cache_get_filling(struct cache_handle *h)
{
	unsigned long percent;

	/* Lock cache access */
	pthread_mutex_lock(&h->mutex);

	/* Check data availability in cache */
	if(h->is_ready)
		percent = 100;
	else
		percent = h->len * 100 / h->size;

	/* Unlock cache access */
	pthread_mutex_unlock(&h->mutex);

	return (unsigned char) percent;
}

static int cache_put_format(struct cache_handle *h, struct a_format *fmt)
{
	struct cache_format *cf;

	/* Allocate format entry */
	cf = malloc(sizeof(struct cache_format));
	if(cf == NULL)
		return -1;

	/* Copy format */
	format_cpy(&cf->fmt, fmt);

	/* Set len before format change */
	cf->len = h->fmt_len;
	cf->next = NULL;

	/* Add to format list */
	if(h->fmt_last != NULL)
		h->fmt_last->next = cf;
	h->fmt_last = cf;
	if(h->fmt_first == NULL)
		h->fmt_first = cf;
	h->fmt_len = 0;

	return 0;
}

static void cache_get_format(struct cache_handle *h)
{
	struct cache_format *cf;

	if(h->fmt_first == NULL)
		return;

	/* Update list */
	cf = h->fmt_first;
	h->fmt_first = cf->next;

	/* Free current entry */
	free(cf);
}

static void *cache_read_thread(void *user_data)
{
	struct cache_handle *h = (struct cache_handle *) user_data;
	struct a_format in_fmt = A_FORMAT_INIT;
	unsigned char buffer[BUFFER_SIZE];
	unsigned long in_size = 0;
	unsigned long len = 0;
	int ret = 0;

	/* Read indefinitively the input callback */
	while(!h->stop)
	{
		/* Lock input callback */
		cache_lock(h);

		/* Flush this buffer */
		if(h->flush)
		{
			h->flush = 0;
			len = 0;
		}

		/* Check buffer len */
		if(len < BUFFER_SIZE / 4)
		{
			/* Read next packet from input callback */
			ret = h->input_callback(h->user_data, &buffer[len*4],
						(BUFFER_SIZE / 4) - len,
						&in_fmt);
			if(ret < 0)
				break;
			len += ret;
		}

		/* Lock cache access */
		pthread_mutex_lock(&h->mutex);

		/* Copy data to cache */
		in_size = h->size - h->len;
		if(in_size > len)
			in_size = len;
		memcpy(&h->buffer[h->len*4], buffer, in_size * 4);
		h->len += in_size;
		len -= in_size;

		/* Update format list */
		if(h->fmt_last == NULL ||
		   ((in_fmt.samplerate != 0 || in_fmt.channels != 0) &&
		    format_cmp(&in_fmt, &h->fmt_last->fmt) != 0))
			cache_put_format(h, &in_fmt);
		h->fmt_len += in_size;

		/* Cache is full */
		if(h->len == h->size)
			h->is_ready = 1;

		/* Unlock cache access */
		pthread_mutex_unlock(&h->mutex);

		/* Move remaining data*/
		if(len > 0)
			memmove(buffer, &buffer[in_size*4], len * 4);

		/* Unlock cache */
		cache_unlock(h);

		/* Buffer is already fill: sleep 1ms */
		if(len >= BUFFER_SIZE / 4)
			usleep(1000);
	}

	return NULL;
}

int cache_read(void *user_data, unsigned char *buffer, size_t size,
	       struct a_format *fmt)
{
	struct cache_handle *h = (struct cache_handle *) user_data;
	struct a_format in_fmt = A_FORMAT_INIT;
	struct cache_format *next_fmt;
	unsigned long in_size;
	long len;

	if(h == NULL || h->buffer == NULL)
		return -1;

	/* Lock cache access */
	pthread_mutex_lock(&h->mutex);

	/* Check data availability in cache */
	if(h->is_ready)
	{
		/* Some data is available */
		if(size > h->len)
			size = h->len;

		/* Check format list */
		if(h->fmt_first != NULL)
		{
			/* Copy current format */
			format_cpy(fmt, &h->fmt_first->fmt);

			/* Check next format */
			next_fmt = h->fmt_first->next;
			if(next_fmt != NULL)
			{
				if(next_fmt->len < size)
				{
					/* Limit size to format switching byte
					 * and free current format
					 */
					size = next_fmt->len;
					cache_get_format(h);
				}
				next_fmt->len -= size;
			}
			else
				h->fmt_len -= size;
		}

		/* Read in cache */
		memcpy(buffer, h->buffer, size*4);
		h->len -= size;
		memmove(h->buffer, &h->buffer[size*4], h->len*4);

		/* No more data is available */
		if(h->len == 0)
			h->is_ready = 0;
	}
	else
		size = 0;

	/* Unlock cache access */
	pthread_mutex_unlock(&h->mutex);

	/* Check cache status */
	if(!h->use_thread && h->len < h->size)
	{
		/* Check input callback access */
		if(pthread_mutex_trylock(&h->input_lock) != 0)
			return size;

		/* Fill cache with some samples */
		in_size = h->size - h->len;
		len = h->input_callback(h->user_data, &h->buffer[h->len*4],
					in_size, &in_fmt);
		if(len < 0)
		{
			if(h->len == 0)
				return -1;
			return size;
		}
		h->len += len;

		/* Update format list */
		if(h->fmt_last == NULL ||
		   ((in_fmt.samplerate != 0 || in_fmt.channels != 0) &&
		    format_cmp(&in_fmt, &h->fmt_last->fmt) != 0))
			cache_put_format(h, &in_fmt);
		h->fmt_len += len;

		/* Cache is full */
		if(h->len == h->size)
			h->is_ready = 1;

		/* Unlock input callback access */
		cache_unlock(h);
	}

	return size;
}

void cache_flush(struct cache_handle *h)
{
	struct cache_format *cf;

	/* Lock input callback */
	cache_lock(h);

	/* Lock cache access */
	pthread_mutex_lock(&h->mutex);

	/* Flush the cache */
	h->is_ready = 0;
	h->len = 0;

	/* Flush format list */
	while(h->fmt_first != NULL)
	{
		cf = h->fmt_first;
		h->fmt_first = cf->next;
		free(cf);
	}
	h->fmt_last = NULL;

	/* Notice flush to thread */
	if(h->use_thread)
		h->flush = 1;

	/* Unlock cache access */
	pthread_mutex_unlock(&h->mutex);
}

void cache_lock(struct cache_handle *h)
{
	/* Lock input callback access */
	pthread_mutex_lock(&h->input_lock);
}

void cache_unlock(struct cache_handle *h)
{
	/* Unlock input callback access */
	pthread_mutex_unlock(&h->input_lock);
}

int cache_close(struct cache_handle *h)
{
	struct cache_format *cf;

	if(h == NULL)
		return 0;

	/* Unlock input callback */
	cache_unlock(h);

	/* Stop thread */
	if(h->use_thread)
	{
		/* Send stop signal */
		h->stop = 1;

		/* Wait end of the thread */
		pthread_join(h->thread, NULL);
	}

	/* Free format list */
	while(h->fmt_first != NULL)
	{
		cf = h->fmt_first;
		h->fmt_first = cf->next;
		free(cf);
	}

	/* Free buffer */
	if(h->buffer != NULL)
		free(h->buffer);

	/* Free structure */
	free(h);

	return 0;
}

