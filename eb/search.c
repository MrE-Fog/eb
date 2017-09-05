/*
 * Copyright (c) 1997, 98, 2000  Motoyuki Kasahara
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <sys/types.h>

#if defined(STDC_HEADERS) || defined(HAVE_STRING_H)
#include <string.h>
#if !defined(STDC_HEADERS) && defined(HAVE_MEMORY_H)
#include <memory.h>
#endif /* not STDC_HEADERS and HAVE_MEMORY_H */
#else /* not STDC_HEADERS and not HAVE_STRING_H */
#include <strings.h>
#endif /* not STDC_HEADERS and not HAVE_STRING_H */

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#ifdef ENABLE_PTHREAD
#include <pthread.h>
#endif

#ifndef HAVE_MEMCPY
#define memcpy(d, s, n) bcopy((s), (d), (n))
#ifdef __STDC__
void *memchr(const void *, int, size_t);
int memcmp(const void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
void *memset(void *, int, size_t);
#else /* not __STDC__ */
char *memchr();
int memcmp();
char *memmove();
char *memset();
#endif /* not __STDC__ */
#endif

#ifndef ENABLE_PTHREAD
#define pthread_mutex_lock(m)
#define pthread_mutex_unlock(m)
#endif

#include "eb.h"
#include "error.h"
#include "internal.h"
#include "text.h"

/*
 * Page-ID macros.
 */
#define PAGE_ID_IS_LEAF_LAYER(page_id)	(((page_id) & 0x80) == 0x80)
#define PAGE_ID_IS_LAYER_START(page_id)	(((page_id) & 0x40) == 0x40)
#define PAGE_ID_IS_LAYER_END(page_id)	(((page_id) & 0x20) == 0x20)
#define PAGE_ID_HAVE_GROUP_ENTRY(page_id)	(((page_id) & 0x10) == 0x10)

/*
 * Book-code of the book in which you want to search a word.
 */
static EB_Book_Code cache_book_code = EB_BOOK_NONE;

/*
 * Cache buffer for the current page.
 */
static char cache_buffer[EB_SIZE_PAGE];

/*
 * Cache buffer for the current page.
 */
static int cache_page;

/*
 * Mutex for cache variables.
 */
#ifdef ENABLE_PTHREAD
static pthread_mutex_t cache_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

/*
 * Unexported functions.
 */
static EB_Error_Code eb_hit_list_word EB_P((EB_Book *, EB_Search_Context *,
    int, EB_Hit *, int *));
static EB_Error_Code eb_hit_list_keyword EB_P((EB_Book *, EB_Search_Context *,
    int, EB_Hit *, int *));
static EB_Error_Code eb_hit_list_multi EB_P((EB_Book *, EB_Search_Context *,
    int, EB_Hit *, int *));
static void eb_and_hit_lists EB_P((EB_Hit [], int *, int, int,
    EB_Hit [EB_NUMBER_OF_SEARCH_CONTEXTS][], int []));

/*
 * Intialize the current search status.
 */
void
eb_initialize_search(book)
    EB_Book *book;
{
    pthread_mutex_lock(&cache_mutex);

    if (book->code == cache_book_code) {
	cache_book_code = EB_BOOK_NONE;
	book->search_contexts[0].code = EB_SEARCH_NONE;
    }

    pthread_mutex_unlock(&cache_mutex);
}


/*
 * Pre-search for a word described in the current search context.
 * It descends intermediate indexes and reached to a leaf page that
 * may have the word.
 * If succeeded, 0 is returned.  Otherwise -1 is returned.
 */
EB_Error_Code
eb_presearch_word(book, context)
    EB_Book *book;
    EB_Search_Context *context;
{
    EB_Error_Code error_code;
    int next_page;
    int index_depth;
    char *cache_p;

    pthread_mutex_lock(&cache_mutex);

    /*
     * Discard cache data.
     */
    cache_book_code = EB_BOOK_NONE;

    /*
     * Search the word in intermediate indexes.
     * Find a page number of the leaf index page.
     */
    for (index_depth = 0; index_depth < EB_MAX_INDEX_DEPTH; index_depth++) {
	next_page = context->page;

	/*
	 * Seek and read a page.
	 */
	if (eb_zlseek(&book->subbook_current->text_zip, 
	    book->subbook_current->text_file,
	    (context->page - 1) * EB_SIZE_PAGE, SEEK_SET) < 0) {
	    cache_book_code = EB_BOOK_NONE;
	    error_code = EB_ERR_FAIL_SEEK_TEXT;
	    goto failed;
	}
	if (eb_zread(&book->subbook_current->text_zip,
	    book->subbook_current->text_file, cache_buffer, EB_SIZE_PAGE)
	    != EB_SIZE_PAGE) {
	    cache_book_code = EB_BOOK_NONE;
	    error_code = EB_ERR_FAIL_READ_TEXT;
	    goto failed;
	}

	/*
	 * Get some data from the read page.
	 */
	context->page_id = eb_uint1(cache_buffer);
	context->entry_length = eb_uint1(cache_buffer + 1);
	if (context->entry_length == 0)
	    context->entry_arrangement = EB_ARRANGE_VARIABLE;
	else
	    context->entry_arrangement = EB_ARRANGE_FIXED;
	context->entry_count = eb_uint2(cache_buffer + 2);
	context->offset = 4;
	cache_p = cache_buffer + 4;

	/*
	 * Exit the loop if it reached to the leaf index.
	 */
	if (PAGE_ID_IS_LEAF_LAYER(context->page_id))
	    break;

	/*
	 * Search a page of next level index.
	 */
	for (context->entry_index = 0;
	     context->entry_index < context->entry_count;
	     context->entry_index++) {
	    if (EB_SIZE_PAGE < context->offset + context->entry_length + 4) {
		error_code = EB_ERR_UNEXP_TEXT;
		goto failed;
	    }
	    if (context->compare(context->canonicalized_word, cache_p,
		context->entry_length) <= 0) {
		next_page = eb_uint4(cache_p + context->entry_length);
		break;
	    }
	    cache_p += context->entry_length + 4;
	    context->offset += context->entry_length + 4;
	}
	if (context->entry_count <= context->entry_index
	    || context->page == next_page) {
	    context->comparison_result = -1;
	    goto succeeded;
	}
	context->page = next_page;
    }

    /*
     * Check for the index depth.
     */
    if (index_depth == EB_MAX_INDEX_DEPTH) {
	error_code = EB_ERR_UNEXP_TEXT;
	goto failed;
    }

    /*
     * Update search context and cache information.
     */
    context->entry_index = 0;
    context->comparison_result = 1;
    context->entry_length = 0;
    context->in_group_entry = 0;
    cache_book_code = book->code;
    cache_page = context->page;

  succeeded:
    pthread_mutex_unlock(&cache_mutex);
    return EB_SUCCESS;

    /*
     * An error occurs...
     */
  failed:
    pthread_mutex_unlock(&cache_mutex);
    return error_code;
}

/*
 * The maximum number of hit entries for tomporary hit lists.
 * This is used in eb_hit_list().
 */
#define EB_TMP_MAX_HITS		64

/*
 * Get hit entries of a submitted search request.
 */
EB_Error_Code
eb_hit_list(book, max_hit_count, hit_list, hit_count)
    EB_Book *book;
    int max_hit_count;
    EB_Hit *hit_list;
    int *hit_count;
{
    EB_Error_Code error_code;
    EB_Search_Context temporary_context;
    EB_Hit temporary_hit_lists[EB_NUMBER_OF_SEARCH_CONTEXTS][EB_TMP_MAX_HITS];
    int temporary_hit_counts[EB_NUMBER_OF_SEARCH_CONTEXTS];
    int more_hit_count;
    int last_temporary_hit_count;
    int i;

    /*
     * Lock cache data and the book.
     */
    pthread_mutex_lock(&cache_mutex);
    eb_lock(&book->lock);

    if (max_hit_count == 0)
	goto succeeded;

    *hit_count = 0;

    /*
     * Current subbook must have been set.
     */
    if (book->subbook_current == NULL) {
	error_code = EB_ERR_NO_CUR_SUB;
	goto failed;
    }

    /*
     * Get a list of hit entries.
     */
    switch (book->search_contexts->code) {
    case EB_SEARCH_EXACTWORD:
    case EB_SEARCH_WORD:
    case EB_SEARCH_ENDWORD:
	/*
	 * In case of exactword, word of endword search.
	 */
	error_code = eb_hit_list_word(book, book->search_contexts,
	    max_hit_count, hit_list, hit_count);
	if (error_code != EB_SUCCESS)
	    goto failed;
	break;

    case EB_SEARCH_KEYWORD:
	/*
	 * In case of keyword search.
	 */
	for (;;) {
	    last_temporary_hit_count = 0;

	    for (i = 0; i < EB_MAX_KEYWORDS; i++) {
		if (book->search_contexts[i].code != EB_SEARCH_KEYWORD)
		    break;
		memcpy(&temporary_context, book->search_contexts + i,
		    sizeof(EB_Search_Context));
		error_code = eb_hit_list_keyword(book, &temporary_context,
		    EB_TMP_MAX_HITS, temporary_hit_lists[i],
		    &last_temporary_hit_count);
		if (error_code != EB_SUCCESS)
		    goto failed;
		temporary_hit_counts[i] = last_temporary_hit_count;
		if (last_temporary_hit_count == 0) {
		    break;
		}
	    }
	    if (last_temporary_hit_count == 0)
		break;

	    eb_and_hit_lists(hit_list + *hit_count, &more_hit_count,
		max_hit_count - *hit_count, i, temporary_hit_lists,
		temporary_hit_counts);

	    for (i = 0; i < EB_MAX_MULTI_ENTRIES; i++) {
		if ((book->search_contexts + i)->code != EB_SEARCH_KEYWORD)
		    break;
		error_code = eb_hit_list_keyword(book,
		    book->search_contexts + i, temporary_hit_counts[i],
		    temporary_hit_lists[i], &last_temporary_hit_count);
		if (error_code != EB_SUCCESS)
		    goto failed;
	    }

	    *hit_count += more_hit_count;
	    if (max_hit_count <= *hit_count)
		break;
	}
	break;

    case EB_SEARCH_MULTI:
	/*
	 * In case of multi search.
	 */
	for (;;) {
	    last_temporary_hit_count = 0;

	    for (i = 0; i < EB_MAX_MULTI_ENTRIES; i++) {
		if (book->search_contexts[i].code != EB_SEARCH_MULTI)
		    break;
		memcpy(&temporary_context, book->search_contexts + i,
		    sizeof(EB_Search_Context));
		error_code = eb_hit_list_multi(book, &temporary_context,
		    EB_TMP_MAX_HITS, temporary_hit_lists[i],
		    &last_temporary_hit_count);
		if (error_code != EB_SUCCESS)
		    goto failed;
		temporary_hit_counts[i] = last_temporary_hit_count;
		if (last_temporary_hit_count == 0) {
		    break;
		}
	    }
	    if (last_temporary_hit_count == 0)
		break;

	    eb_and_hit_lists(hit_list + *hit_count, &more_hit_count,
		max_hit_count - *hit_count, i, temporary_hit_lists,
		temporary_hit_counts);

	    for (i = 0; i < EB_MAX_MULTI_ENTRIES; i++) {
		if ((book->search_contexts + i)->code != EB_SEARCH_MULTI)
		    break;
		error_code = eb_hit_list_multi(book,
		    book->search_contexts + i, temporary_hit_counts[i],
		    temporary_hit_lists[i], &last_temporary_hit_count);
		if (error_code != EB_SUCCESS)
		    goto failed;
	    }

	    *hit_count += more_hit_count;
	    if (max_hit_count <= *hit_count)
		break;
	}
	break;

    default:
	/* not reached */
	error_code = EB_ERR_NO_PREV_SEARCH;
	goto failed;
    }

    /*
     * Unlock cache data and the book.
     */
  succeeded:
    eb_unlock(&book->lock);
    pthread_mutex_unlock(&cache_mutex);

    return EB_SUCCESS;

    /*
     * An error occurs...
     */
  failed:
    *hit_count = 0;
    eb_unlock(&book->lock);
    pthread_mutex_unlock(&cache_mutex);
    return error_code;
}


/*
 * Get hit entries of a submitted exactword/word/endword search request.
 */
static EB_Error_Code
eb_hit_list_word(book, context, max_hit_count, hit_list, hit_count)
    EB_Book *book;
    EB_Search_Context *context;
    int max_hit_count;
    EB_Hit *hit_list;
    int *hit_count;
{
    EB_Error_Code error_code;
    EB_Hit *hit = hit_list;
    int group_id;
    char *cache_p;

    *hit_count = 0;

    /*
     * If the result of previous comparison is negative value, all
     * matched entries have been found.
     */
    if (context->comparison_result < 0)
	goto succeeded;

    for (;;) {
	/*
	 * Read a page to search, if the page is not on the cache buffer.
	 *
	 * Cache may be missed by the two reasons:
	 *   1. the search process reaches to the end of an index page,
	 *      and tries to read the next page.
	 *   2. Someone else used the cache data.
	 * 
	 * At the case of 1, the search process reads the page and update
	 * the search context.  At the case of 2. it reads the page but
	 * muts not update the context!
	 */
	if (cache_book_code != book->code || cache_page != context->page) {
	    if (eb_zlseek(&book->subbook_current->text_zip,
		book->subbook_current->text_file,
		(context->page - 1) * EB_SIZE_PAGE, SEEK_SET) < 0) {
		error_code = EB_ERR_FAIL_SEEK_TEXT;
		goto failed;
	    }
	    if (eb_zread(&book->subbook_current->text_zip,
		book->subbook_current->text_file, cache_buffer, EB_SIZE_PAGE)
		!= EB_SIZE_PAGE) {
		error_code = EB_ERR_FAIL_READ_TEXT;
		goto failed;
	    }

	    /*
	     * Update search context.
	     */
	    if (context->entry_index == 0) {
		context->page_id = eb_uint1(cache_buffer);
		context->entry_length = eb_uint1(cache_buffer + 1);
		if (context->entry_length == 0)
		    context->entry_arrangement = EB_ARRANGE_VARIABLE;
		else
		    context->entry_arrangement = EB_ARRANGE_FIXED;
		context->entry_count = eb_uint2(cache_buffer + 2);
		context->entry_index = 0;
		context->offset = 4;
	    }

	    cache_book_code = book->code;
	    cache_page = context->page;
	}

	cache_p = cache_buffer + context->offset;

	if (!PAGE_ID_IS_LEAF_LAYER(context->page_id)) {
	    /*
	     * Not a leaf index.  It is an error.
	     */
	    error_code = EB_ERR_UNEXP_TEXT;
	    goto failed;
	}

	if (!PAGE_ID_HAVE_GROUP_ENTRY(context->page_id)
	    && context->entry_arrangement == EB_ARRANGE_FIXED) {
	    /*
	     * The leaf index doesn't have a group entry.
	     * Find text and heading locations.
	     */
	    while (context->entry_index < context->entry_count) {
		if (EB_SIZE_PAGE
		    < context->offset + context->entry_length + 12) {
		    error_code = EB_ERR_UNEXP_TEXT;
		    goto failed;
		}

		/*
		 * Compare word and pattern.
		 * If matched, add it to a hit list.
		 */
		context->comparison_result
		    = context->compare(context->word, cache_p,
			context->entry_length);
		if (context->comparison_result == 0) {
		    hit->heading.page
			= eb_uint4(cache_p + context->entry_length + 6);
		    hit->heading.offset
			= eb_uint2(cache_p + context->entry_length + 10);
		    hit->text.page
			= eb_uint4(cache_p + context->entry_length);
		    hit->text.offset
			= eb_uint2(cache_p + context->entry_length + 4);
		    hit++;
		    *hit_count += 1;
		}
		context->entry_index++;
		context->offset += context->entry_length + 12;
		cache_p += context->entry_length + 12;

		if (context->comparison_result < 0
		    || max_hit_count <= *hit_count)
		    goto succeeded;
	    }

	} else if (!PAGE_ID_HAVE_GROUP_ENTRY(context->page_id)
	    && context->entry_arrangement == EB_ARRANGE_VARIABLE) {

	    /*
	     * The leaf index doesn't have a group entry.
	     * Find text and heading locations.
	     */
	    while (context->entry_index < context->entry_count) {
		if (EB_SIZE_PAGE < context->offset + 1) {
		    error_code = EB_ERR_UNEXP_TEXT;
		    goto failed;
		}
		context->entry_length = eb_uint1(cache_p);
		if (EB_SIZE_PAGE
		    < context->offset + context->entry_length + 13) {
		    error_code = EB_ERR_UNEXP_TEXT;
		    goto failed;
		}

		/*
		 * Compare word and pattern.
		 * If matched, add it to a hit list.
		 */
		context->comparison_result
		    = context->compare(context->word, cache_p + 1,
			context->entry_length);
		if (context->comparison_result == 0) {
		    hit->heading.page
			= eb_uint4(cache_p + context->entry_length + 7);
		    hit->heading.offset
			= eb_uint2(cache_p + context->entry_length + 11);
		    hit->text.page
			= eb_uint4(cache_p + context->entry_length + 1);
		    hit->text.offset
			= eb_uint2(cache_p + context->entry_length + 5);
		    hit++;
		    *hit_count += 1;
		}
		context->entry_index++;
		context->offset += context->entry_length + 13;
		cache_p += context->entry_length + 13;

		if (context->comparison_result < 0
		    || max_hit_count <= *hit_count)
		    goto succeeded;
	    }

	} else {
	    /*
	     * The leaf index have a group entry.
	     * Find text and heading locations.
	     */
	    while (context->entry_index < context->entry_count) {
		if (EB_SIZE_PAGE < context->offset + 2) {
		    error_code = EB_ERR_UNEXP_TEXT;
		    goto failed;
		}
		group_id = eb_uint1(cache_p);

		if (group_id == 0x00) {
		    /*
		     * 0x00 -- Single entry.
		     */
		    context->entry_length = eb_uint1(cache_p + 1);
		    if (EB_SIZE_PAGE
			< context->offset + context->entry_length + 14) {
			error_code = EB_ERR_UNEXP_TEXT;
			goto failed;
		    }

		    /*
		     * Compare word and pattern.
		     * If matched, add it to a hit list.
		     */
		    context->comparison_result
			= context->compare(context->canonicalized_word,
			    cache_p + 2, context->entry_length);
		    if (context->comparison_result == 0
			&& context->compare(context->word, cache_p + 2,
			    context->entry_length) == 0) {
			hit->heading.page
			    = eb_uint4(cache_p + context->entry_length + 8);
			hit->heading.offset
			    = eb_uint2(cache_p + context->entry_length + 12);
			hit->text.page
			    = eb_uint4(cache_p + context->entry_length + 2);
			hit->text.offset
			    = eb_uint2(cache_p + context->entry_length + 6);
			hit++;
			*hit_count += 1;
		    }
		    context->in_group_entry = 0;
		    context->offset += context->entry_length + 14;
		    cache_p += context->entry_length + 14;

		} else if (group_id == 0x80) {
		    /*
		     * 0x80 -- Start of group entry.
		     */
		    context->entry_length = eb_uint1(cache_p + 1);
		    if (EB_SIZE_PAGE
			< context->offset + context->entry_length + 4) {
			error_code = EB_ERR_UNEXP_TEXT;
			goto failed;
		    }
		    context->comparison_result
			= context->compare(context->canonicalized_word,
			    cache_p + 4, context->entry_length);
		    context->in_group_entry = 1;
		    cache_p += context->entry_length + 4;
		    context->offset += context->entry_length + 4;

		} else if (group_id == 0xc0) {
		    /*
		     * Element of the group entry
		     */
		    context->entry_length = eb_uint1(cache_p + 1);
		    if (EB_SIZE_PAGE < context->offset + 14) {
			error_code = EB_ERR_UNEXP_TEXT;
			goto failed;
		    }

		    /*
		     * Compare word and pattern.
		     * If matched, add it to a hit list.
		     */
		    if (context->comparison_result == 0
			&& context->in_group_entry
			&& context->compare(context->word, cache_p + 2,
			    context->entry_length) == 0) {
			hit->heading.page
			    = eb_uint4(cache_p + context->entry_length + 8);
			hit->heading.offset
			    = eb_uint2(cache_p + context->entry_length + 12);
			hit->text.page
			    = eb_uint4(cache_p + context->entry_length + 2);
			hit->text.offset
			    = eb_uint2(cache_p + context->entry_length + 6);
			hit++;
			*hit_count += 1;
		    }
		    context->offset += context->entry_length + 14;
		    cache_p += context->entry_length + 14;

		} else {
		    /*
		     * Unknown group ID.
		     */
		    error_code = EB_ERR_UNEXP_TEXT;
		    goto failed;
		}

		context->entry_index++;
		if (context->comparison_result < 0
		    || max_hit_count <= *hit_count)
		    goto succeeded;
	    }
	}

	/*
	 * Go to a next page if available.
	 */
	if (PAGE_ID_IS_LAYER_END(context->page_id)) {
	    context->comparison_result = -1;
	    goto succeeded;
	}
	context->page++;
	context->entry_index = 0;
    }

  succeeded:
    return EB_SUCCESS;

    /*
     * An error occurs...
     * Discard cache if read error occurs.
     */
  failed:
    if (error_code == EB_ERR_FAIL_READ_TEXT)
	cache_book_code = EB_BOOK_NONE;
    *hit_count = 0;
    return error_code;
}


/*
 * Get hit entries of a submitted keyword search request.
 */
static EB_Error_Code
eb_hit_list_keyword(book, context, max_hit_count, hit_list, hit_count)
    EB_Book *book;
    EB_Search_Context *context;
    int max_hit_count;
    EB_Hit *hit_list;
    int *hit_count;
{
    EB_Error_Code error_code;
    EB_Text_Context text_context;
    EB_Hit *hit = hit_list;
    int group_id;
    char *cache_p;

    *hit_count = 0;

    /*
     * Backup the text context in `book'.
     */
    memcpy(&text_context, &book->text_context, sizeof(EB_Text_Context));

    /*
     * Seek text file.
     */
    if (context->in_group_entry && context->comparison_result == 0) {
	error_code = eb_seek_text(book, &context->keyword_heading);
	if (error_code != EB_SUCCESS)
	    goto failed;
    }

    /*
     * If the result of previous comparison is negative value, all
     * matched entries have been found.
     */
    if (context->comparison_result < 0)
	goto succeeded;

    for (;;) {
	/*
	 * Read a page to search, if the page is not on the cache buffer.
	 *
	 * Cache may be missed by the two reasons:
	 *   1. the search process reaches to the end of an index page,
	 *      and tries to read the next page.
	 *   2. Someone else used the cache data.
	 * 
	 * At the case of 1, the search process reads the page and update
	 * the search context.  At the case of 2. it reads the page but
	 * muts not update the context!
	 */
	if (cache_book_code != book->code || cache_page != context->page) {
	    if (eb_zlseek(&book->subbook_current->text_zip,
		book->subbook_current->text_file,
		(context->page - 1) * EB_SIZE_PAGE, SEEK_SET) < 0) {
		error_code = EB_ERR_FAIL_SEEK_TEXT;
		goto failed;
	    }
	    if (eb_zread(&book->subbook_current->text_zip,
		book->subbook_current->text_file, cache_buffer, EB_SIZE_PAGE)
		!= EB_SIZE_PAGE) {
		error_code = EB_ERR_FAIL_READ_TEXT;
		goto failed;
	    }

	    /*
	     * Update search context.
	     */
	    if (context->entry_index == 0) {
		context->page_id = eb_uint1(cache_buffer);
		context->entry_length = eb_uint1(cache_buffer + 1);
		if (context->entry_length == 0)
		    context->entry_arrangement = EB_ARRANGE_VARIABLE;
		else
		    context->entry_arrangement = EB_ARRANGE_FIXED;
		context->entry_count = eb_uint2(cache_buffer + 2);
		context->entry_index = 0;
		context->offset = 4;
	    }

	    cache_book_code = book->code;
	    cache_page = context->page;
	}

	cache_p = cache_buffer + context->offset;

	if (!PAGE_ID_IS_LEAF_LAYER(context->page_id)) {
	    /*
	     * Not a leaf index.  It is an error.
	     */
	    error_code = EB_ERR_UNEXP_TEXT;
	    goto failed;
	}

	if (!PAGE_ID_HAVE_GROUP_ENTRY(context->page_id)
	    && context->entry_arrangement == EB_ARRANGE_FIXED) {
	    /*
	     * The leaf index doesn't have a group entry.
	     * Find text and heading locations.
	     */
	    while (context->entry_index < context->entry_count) {
		if (EB_SIZE_PAGE
		    < context->offset + context->entry_length + 12) {
		    error_code = EB_ERR_UNEXP_TEXT;
		    goto failed;
		}

		/*
		 * Compare word and pattern.
		 * If matched, add it to a hit list.
		 */
		context->comparison_result
		    = context->compare(context->word, cache_p,
			context->entry_length);
		if (context->comparison_result == 0) {
		    hit->heading.page
			= eb_uint4(cache_p + context->entry_length + 6);
		    hit->heading.offset
			= eb_uint2(cache_p + context->entry_length + 10);
		    hit->text.page
			= eb_uint4(cache_p + context->entry_length);
		    hit->text.offset
			= eb_uint2(cache_p + context->entry_length + 4);
		    hit++;
		    *hit_count += 1;
		}
		context->entry_index++;
		context->offset += context->entry_length + 12;
		cache_p += context->entry_length + 12;

		if (context->comparison_result < 0
		    || max_hit_count <= *hit_count)
		    goto succeeded;
	    }

	} else if (!PAGE_ID_HAVE_GROUP_ENTRY(context->page_id)
	    && context->entry_arrangement == EB_ARRANGE_VARIABLE) {
	    /*
	     * The leaf index doesn't have a group entry.
	     * Find text and heading locations.
	     */
	    while (context->entry_index < context->entry_count) {
		if (EB_SIZE_PAGE < context->offset + 1) {
		    error_code = EB_ERR_UNEXP_TEXT;
		    goto failed;
		}
		context->entry_length = eb_uint1(cache_p);
		if (EB_SIZE_PAGE
		    < context->offset + context->entry_length + 13) {
		    error_code = EB_ERR_UNEXP_TEXT;
		    goto failed;
		}

		/*
		 * Compare word and pattern.
		 * If matched, add it to a hit list.
		 */
		context->comparison_result
		    = context->compare(context->word, cache_p + 1,
			context->entry_length);
		if (context->comparison_result == 0) {
		    hit->heading.page
			= eb_uint4(cache_p + context->entry_length + 7);
		    hit->heading.offset
			= eb_uint2(cache_p + context->entry_length + 11);
		    hit->text.page
			= eb_uint4(cache_p + context->entry_length + 1);
		    hit->text.offset
			= eb_uint2(cache_p + context->entry_length + 5);
		    hit++;
		    *hit_count += 1;
		}
		context->entry_index++;
		context->offset += context->entry_length + 13;
		cache_p += context->entry_length + 13;

		if (context->comparison_result < 0
		    || max_hit_count <= *hit_count)
		    goto succeeded;
	    }

	} else {
	    /*
	     * The leaf index have a group entry.
	     * Find text and heading locations.
	     */
	    while (context->entry_index < context->entry_count) {
		if (EB_SIZE_PAGE < context->offset + 2) {
		    error_code = EB_ERR_UNEXP_TEXT;
		    goto failed;
		}
		group_id = eb_uint1(cache_p);

		if (group_id == 0x00) {
		    /*
		     * 0x00 -- Single entry.
		     */
		    context->entry_length = eb_uint1(cache_p + 1);
		    if (EB_SIZE_PAGE
			< context->offset + context->entry_length + 14) {
			error_code = EB_ERR_UNEXP_TEXT;
			goto failed;
		    }

		    /*
		     * Compare word and pattern.
		     * If matched, add it to a hit list.
		     */
		    context->comparison_result
			= context->compare(context->canonicalized_word,
			    cache_p + 2, context->entry_length);
		    if (context->comparison_result == 0
			&& context->compare(context->word, cache_p + 2,
			    context->entry_length) == 0) {
			hit->heading.page
			    = eb_uint4(cache_p + context->entry_length + 8);
			hit->heading.offset
			    = eb_uint2(cache_p + context->entry_length + 12);
			hit->text.page
			    = eb_uint4(cache_p + context->entry_length + 2);
			hit->text.offset
			    = eb_uint2(cache_p + context->entry_length + 6);
			hit++;
			*hit_count += 1;
		    }
		    context->in_group_entry = 0;
		    context->offset += context->entry_length + 14;
		    cache_p += context->entry_length + 14;

		} else if (group_id == 0x80) {
		    /*
		     * 0x80 -- Start of group entry.
		     */
		    context->entry_length = eb_uint1(cache_p + 1);
		    if (EB_SIZE_PAGE
			< context->offset + context->entry_length + 12) {
			error_code = EB_ERR_UNEXP_TEXT;
			goto failed;
		    }
		    context->comparison_result
			= context->compare(context->canonicalized_word,
			    cache_p + 6, context->entry_length);
		    context->keyword_heading.page
			= eb_uint4(cache_p + context->entry_length + 6);
		    context->keyword_heading.offset
			= eb_uint2(cache_p + context->entry_length + 10);
		    context->in_group_entry = 1;
		    cache_p += context->entry_length + 12;
		    context->offset += context->entry_length + 12;

		    if (context->comparison_result == 0) {
			error_code
			    = eb_seek_text(book, &context->keyword_heading);
			if (error_code != EB_SUCCESS)
			    goto failed;
		    }

		} else if (group_id == 0xc0) {
		    /*
		     * Element of the group entry.
		     */
		    if (EB_SIZE_PAGE < context->offset + 7) {
			error_code = EB_ERR_UNEXP_TEXT;
			goto failed;
		    }

		    /*
		     * Compare word and pattern.
		     * If matched, add it to a hit list.
		     */
		    if (context->in_group_entry
			&& context->comparison_result == 0) {
			error_code
			    = eb_tell_text(book, &context->keyword_heading);
			if (error_code != EB_SUCCESS)
			    goto failed;
			hit->heading.page   = context->keyword_heading.page;
			hit->heading.offset = context->keyword_heading.offset;
			hit->text.page      = eb_uint4(cache_p + 1);
			hit->text.offset    = eb_uint2(cache_p + 5);
			hit++;
			*hit_count += 1;
			error_code = eb_forward_heading(book);
			if (error_code != EB_SUCCESS)
			    goto failed;
		    }
		    context->offset += 7;
		    cache_p += 7;
		    
		} else {
		    /*
		     * Unknown group ID.
		     */
		    error_code = EB_ERR_UNEXP_TEXT;
		    goto failed;
		}

		context->entry_index++;
		if (context->comparison_result < 0
		    || max_hit_count <= *hit_count)
		    goto succeeded;
	    }
	}

	/*
	 * Go to a next page if available.
	 */
	if (PAGE_ID_IS_LAYER_END(context->page_id)) {
	    context->comparison_result = -1;
	    goto succeeded;
	}
	context->page++;
	context->entry_index = 0;
    }

  succeeded:
    if (context->in_group_entry && context->comparison_result == 0) {
	error_code = eb_tell_text(book, &context->keyword_heading);
	if (error_code != EB_SUCCESS)
	    goto failed;
    }

    /*
     * Restore the text context in `book'.
     */
    memcpy(&book->text_context, &text_context, sizeof(EB_Text_Context));
    return EB_SUCCESS;

    /*
     * An error occurs...
     * Discard cache if read error occurs.
     */
  failed:
    if (error_code == EB_ERR_FAIL_READ_TEXT)
	cache_book_code = EB_BOOK_NONE;
    *hit_count = 0;
    memcpy(&book->text_context, &text_context, sizeof(EB_Text_Context));
    return error_code;
}


/*
 * Get hit entries of a submitted multi search request.
 */
static EB_Error_Code
eb_hit_list_multi(book, context, max_hit_count, hit_list, hit_count)
    EB_Book *book;
    EB_Search_Context *context;
    int max_hit_count;
    EB_Hit *hit_list;
    int *hit_count;
{
    EB_Error_Code error_code;
    EB_Hit *hit = hit_list;
    int group_id;
    char *cache_p;

    *hit_count = 0;

    /*
     * If the result of previous comparison is negative value, all
     * matched entries have been found.
     */
    if (context->comparison_result < 0)
	return 0;

    for (;;) {
	/*
	 * Read a page to search, if the page is not on the cache buffer.
	 *
	 * Cache may be missed by the two reasons:
	 *   1. the search process reaches to the end of an index page,
	 *      and tries to read the next page.
	 *   2. Someone else used the cache data.
	 * 
	 * At the case of 1, the search process reads the page and update
	 * the search context.  At the case of 2. it reads the page but
	 * muts not update the context!
	 */
	if (cache_book_code != book->code || cache_page != context->page) {
	    if (eb_zlseek(&book->subbook_current->text_zip,
		book->subbook_current->text_file,
		(context->page - 1) * EB_SIZE_PAGE, SEEK_SET) < 0) {
		error_code = EB_ERR_FAIL_SEEK_TEXT;
		goto failed;
	    }
	    if (eb_zread(&book->subbook_current->text_zip,
		book->subbook_current->text_file, cache_buffer, EB_SIZE_PAGE)
		!= EB_SIZE_PAGE) {
		error_code = EB_ERR_FAIL_READ_TEXT;
		goto failed;
	    }

	    /*
	     * Update search context.
	     */
	    if (context->entry_index == 0) {
		context->page_id = eb_uint1(cache_buffer);
		context->entry_length = eb_uint1(cache_buffer + 1);
		if (context->entry_length == 0)
		    context->entry_arrangement = EB_ARRANGE_VARIABLE;
		else
		    context->entry_arrangement = EB_ARRANGE_FIXED;
		context->entry_count = eb_uint2(cache_buffer + 2);
		context->entry_index = 0;
		context->offset = 4;
	    }

	    cache_book_code = book->code;
	    cache_page = context->page;
	}

	cache_p = cache_buffer + context->offset;

	if (!PAGE_ID_IS_LEAF_LAYER(context->page_id)) {
	    /*
	     * Not a leaf index.  It is an error.
	     */
	    error_code = EB_ERR_UNEXP_TEXT;
	    goto failed;
	}

	if (!PAGE_ID_HAVE_GROUP_ENTRY(context->page_id)
	    && context->entry_arrangement == EB_ARRANGE_FIXED) {
	    /*
	     * The leaf index doesn't have a group entry.
	     * Find text and heading locations.
	     */
	    while (context->entry_index < context->entry_count) {
		if (EB_SIZE_PAGE
		    < context->offset + context->entry_length + 13) {
		    error_code = EB_ERR_UNEXP_TEXT;
		    goto failed;
		}

		/*
		 * Compare word and pattern.
		 * If matched, add it to a hit list.
		 */
		context->comparison_result
		    = context->compare(context->word, cache_p,
			context->entry_length);
		if (context->comparison_result == 0) {
		    hit->heading.page
			= eb_uint4(cache_p + context->entry_length + 6);
		    hit->heading.offset
			= eb_uint2(cache_p + context->entry_length + 10);
		    hit->text.page
			= eb_uint4(cache_p + context->entry_length);
		    hit->text.offset
			= eb_uint2(cache_p + context->entry_length + 4);
		    hit++;
		    *hit_count += 1;
		}
		context->entry_index++;
		context->offset += context->entry_length + 12;
		cache_p += context->entry_length + 12;

		if (context->comparison_result < 0
		    || max_hit_count <= *hit_count)
		    goto succeeded;
	    }

	} else if (!PAGE_ID_HAVE_GROUP_ENTRY(context->page_id)
	    && context->entry_arrangement == EB_ARRANGE_VARIABLE) {
	    /*
	     * The leaf index doesn't have a group entry.
	     * Find text and heading locations.
	     */
	    while (context->entry_index < context->entry_count) {
		if (EB_SIZE_PAGE < context->offset + 1) {
		    error_code = EB_ERR_UNEXP_TEXT;
		    goto failed;
		}
		context->entry_length = eb_uint1(cache_p);
		if (EB_SIZE_PAGE
		    < context->offset + context->entry_length + 13) {
		    error_code = EB_ERR_UNEXP_TEXT;
		    goto failed;
		}

		/*
		 * Compare word and pattern.
		 * If matched, add it to a hit list.
		 */
		context->comparison_result
		    = context->compare(context->word, cache_p + 1,
			context->entry_length);
		if (context->comparison_result == 0) {
		    hit->heading.page
			= eb_uint4(cache_p + context->entry_length + 7);
		    hit->heading.offset
			= eb_uint2(cache_p + context->entry_length + 11);
		    hit->text.page
			= eb_uint4(cache_p + context->entry_length + 1);
		    hit->text.offset
			= eb_uint2(cache_p + context->entry_length + 5);
		    hit++;
		    *hit_count += 1;
		}
		context->entry_index++;
		context->offset += context->entry_length + 13;
		cache_p += context->entry_length + 13;

		if (context->comparison_result < 0
		    || max_hit_count <= *hit_count)
		    goto succeeded;
	    }

	} else {
	    /*
	     * The leaf index have a group entry.
	     * Find text and heading locations.
	     */
	    while (context->entry_index < context->entry_count) {
		if (EB_SIZE_PAGE < context->offset + 2) {
		    error_code = EB_ERR_UNEXP_TEXT;
		    goto failed;
		}
		group_id = eb_uint1(cache_p);

		if (group_id == 0x00) {
		    /*
		     * 0x00 -- Single entry.
		     */
		    context->entry_length = eb_uint1(cache_p + 1);
		    if (EB_SIZE_PAGE
			< context->offset + context->entry_length + 14) {
			error_code = EB_ERR_UNEXP_TEXT;
			goto failed;
		    }

		    /*
		     * Compare word and pattern.
		     * If matched, add it to a hit list.
		     */
		    context->comparison_result
			= context->compare(context->canonicalized_word,
			    cache_p + 2, context->entry_length);
		    if (context->comparison_result == 0
			&& context->compare(context->word, cache_p + 2,
			    context->entry_length) == 0) {
			hit->heading.page
			    = eb_uint4(cache_p + context->entry_length + 8);
			hit->heading.offset
			    = eb_uint2(cache_p + context->entry_length + 12);
			hit->text.page
			    = eb_uint4(cache_p + context->entry_length + 2);
			hit->text.offset
			    = eb_uint2(cache_p + context->entry_length + 6);
			hit++;
			*hit_count += 1;
		    }
		    context->in_group_entry = 0;
		    context->offset += context->entry_length + 14;
		    cache_p += context->entry_length + 14;

		} else if (group_id == 0x80) {
		    /*
		     * 0x80 -- Start of group entry.
		     */
		    context->entry_length = eb_uint1(cache_p + 1);
		    if (EB_SIZE_PAGE
			< context->offset + context->entry_length + 6) {
			error_code = EB_ERR_UNEXP_TEXT;
			goto failed;
		    }
		    context->comparison_result
			= context->compare(context->canonicalized_word,
			    cache_p + 6, context->entry_length);
		    context->in_group_entry = 1;
		    cache_p += context->entry_length + 6;
		    context->offset += context->entry_length + 6;

		} else if (group_id == 0xc0) {
		    /*
		     * Element of the group entry.
		     */
		    if (EB_SIZE_PAGE < context->offset + 13) {
			error_code = EB_ERR_UNEXP_TEXT;
			goto failed;
		    }

		    /*
		     * Compare word and pattern.
		     * If matched, add it to a hit list.
		     */
		    if (context->in_group_entry
			&& context->comparison_result == 0) {
			hit->heading.page   = eb_uint4(cache_p + 7);
			hit->heading.offset = eb_uint2(cache_p + 11);
			hit->text.page      = eb_uint4(cache_p + 1);
			hit->text.offset    = eb_uint2(cache_p + 5);
			hit++;
			*hit_count += 1;
		    }
		    context->offset += 13;
		    cache_p += 13;
		    
		} else {
		    /*
		     * Unknown group ID.
		     */
		    error_code = EB_ERR_UNEXP_TEXT;
		    goto failed;
		}

		context->entry_index++;
		if (context->comparison_result < 0
		    || max_hit_count <= *hit_count)
		    goto succeeded;
	    }
	}

	/*
	 * Go to a next page if available.
	 */
	if (PAGE_ID_IS_LAYER_END(context->page_id)) {
	    context->comparison_result = -1;
	    goto succeeded;
	}
	context->page++;
	context->entry_index = 0;
    }

  succeeded:
    return EB_SUCCESS;

    /*
     * An error occurs...
     * Discard cache if read error occurs.
     */
  failed:
    if (error_code == EB_ERR_FAIL_READ_TEXT)
	cache_book_code = EB_BOOK_NONE;
    *hit_count = 0;
    return error_code;
}


/*
 * Do AND operation of `hit_lists'.
 */
static void
eb_and_hit_lists(and_list, and_count, max_and_count, hit_list_count,
    hit_lists, hit_counts)
    EB_Hit and_list[EB_TMP_MAX_HITS];
    int *and_count;
    int max_and_count;
    int hit_list_count;
    EB_Hit hit_lists[EB_NUMBER_OF_SEARCH_CONTEXTS][EB_TMP_MAX_HITS];
    int hit_counts[EB_NUMBER_OF_SEARCH_CONTEXTS];
{
    int hit_indexes[EB_NUMBER_OF_SEARCH_CONTEXTS];
    int greatest_list;
    int greatest_page;
    int greatest_offset;
    int current_page;
    int current_offset;
    int equal_count;
    int increment_count;
    int i;

    /*
     * Initialize indexes for the hit_lists[].
     */
    for (i = 0; i < hit_list_count; i++)
	hit_indexes[i] = 0;

    /*
     * Generate the new list `and_list'.
     */
    *and_count = 0;
    while (*and_count < max_and_count) {
	/*
	 * Initialize variables.
	 */
	greatest_list = -1;
	greatest_page = 0;
	greatest_offset = 0;
	current_page = 0;
	current_offset = 0;
	equal_count = 0;

	/*
	 * Compare the current elements of the lists.
	 */
	for (i = 0; i < hit_list_count; i++) {
	    /*
	     * If we have been reached to the tail of the hit_lists[i],
	     * skip the list.
	     */
	    if (hit_counts[i] <= hit_indexes[i])
		continue;

	    /*
	     * Compare {current_page, current_offset} and {greatest_page,
	     * greatest_offset}.
	     */
	    current_page = hit_lists[i][hit_indexes[i]].text.page;
	    current_offset = hit_lists[i][hit_indexes[i]].text.offset;

	    if (greatest_list == -1) {
		greatest_page = current_page;
		greatest_offset = current_offset;
		greatest_list = i;
		equal_count++;
	    } else if (greatest_page < current_page) {
		greatest_page = current_page;
		greatest_offset = current_offset;
		greatest_list = i;
	    } else if (current_page == greatest_page
		&& greatest_offset < current_offset) {
		greatest_page = current_page;
		greatest_offset = current_offset;
		greatest_list = i;
	    } else if (current_page == greatest_page
		&& current_offset == greatest_offset) {
		equal_count++;
	    }
	}

	if (equal_count == hit_list_count) {
	    /*
	     * All the current elements of the lists point to the same
	     * position.  This is hit element.  Increase indexes of all
	     * lists.
	     */
	    memcpy(and_list + *and_count, hit_lists[0] + hit_indexes[0],
		sizeof(EB_Hit));
	    *and_count += 1;
	    for (i = 0; i < hit_list_count; i++) {
		if (hit_counts[i] <= hit_indexes[i])
		    continue;
		hit_indexes[i]++;
	    }
	} else {
	    /*
	     * This is not hit element.  Increase indexes of all lists
	     * except for greatest element(s).  If no index is incremented,
	     * our job has been completed.
	     */
	    increment_count = 0;
	    for (i = 0; i < hit_list_count; i++) {
		if (hit_counts[i] <= hit_indexes[i])
		    continue;
		current_page = hit_lists[i][hit_indexes[i]].text.page;
		current_offset = hit_lists[i][hit_indexes[i]].text.offset;
		if (current_page != greatest_page
		    || current_offset != greatest_offset) {
		    hit_indexes[i]++;
		    increment_count++;
		}
	    }
	    if (increment_count == 0)
		break;
	}
    }

    /*
     * Update hit_counts[].
     * The hit counts of the lists are set to the current indexes.
     */
    for (i = 0; i < hit_list_count; i++)
	hit_counts[i] = hit_indexes[i];
}
