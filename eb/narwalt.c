/*
 * Copyright (c) 1997, 98, 99, 2000  Motoyuki Kasahara
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

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#ifdef ENABLE_PTHREAD
#include <pthread.h>
#endif

#include "eb.h"
#include "error.h"
#include "appendix.h"
#include "internal.h"

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

/*
 * Unexported functions.
 */
static EB_Error_Code eb_narrow_character_text_jis EB_P((EB_Appendix *, int,
    char *));
static EB_Error_Code eb_narrow_character_text_latin EB_P((EB_Appendix *, int,
    char *));

/*
 * Hash macro for cache data.
 */
#define EB_HASH_ALT_CACHE(c)	((c) & 0x0f)


/*
 * Examine whether the current subbook in `book' has a narrow font
 * alternation or not.
 */
int
eb_have_narrow_alt(appendix)
    EB_Appendix *appendix;
{
    /*
     * Lock the appendix.
     */
    eb_lock(&appendix->lock);

    /*
     * Current subbook must have been set.
     */
    if (appendix->subbook_current == NULL)
	goto failed;

    if (appendix->subbook_current->narrow_page == 0)
	goto failed;

    /*
     * Unlock the appendix.
     */
    eb_unlock(&appendix->lock);

    return 1;

    /*
     * An error occurs...
     */
  failed:
    eb_unlock(&appendix->lock);
    return 0;
}


/*
 * Look up the character number of the start of the narrow font alternation
 * of the current subbook in `book'.
 */
EB_Error_Code
eb_narrow_alt_start(appendix, start)
    EB_Appendix *appendix;
    int *start;
{
    EB_Error_Code error_code;

    /*
     * Lock the appendix.
     */
    eb_lock(&appendix->lock);

    /*
     * Current subbook must have been set.
     */
    if (appendix->subbook_current == NULL) {
	error_code = EB_ERR_NO_CUR_APPSUB;
	goto failed;
    }

    if (appendix->subbook_current->narrow_page == 0) {
	error_code = EB_ERR_NO_SUCH_CHAR_TEXT;
	goto failed;
    }

    *start = appendix->subbook_current->narrow_start;

    /*
     * Unlock the appendix.
     */
    eb_unlock(&appendix->lock);

    return EB_SUCCESS;

    /*
     * An error occurs...
     */
  failed:
    *start = -1;
    eb_unlock(&appendix->lock);
    return error_code;
}


/*
 * Return the character number of the end of the narrow font alternation
 * of the current subbook in `book'.
 */
EB_Error_Code
eb_narrow_alt_end(appendix, end)
    EB_Appendix *appendix;
    int *end;
{
    EB_Error_Code error_code;

    /*
     * Lock the appendix.
     */
    eb_lock(&appendix->lock);

    /*
     * Current subbook must have been set.
     */
    if (appendix->subbook_current == NULL) {
	error_code = EB_ERR_NO_CUR_APPSUB;
	goto failed;
    }

    if (appendix->subbook_current->narrow_page == 0) {
	error_code = EB_ERR_NO_SUCH_CHAR_TEXT;
	goto failed;
    }

    *end = appendix->subbook_current->narrow_end;

    /*
     * Unlock the appendix.
     */
    eb_unlock(&appendix->lock);

    return EB_SUCCESS;

    /*
     * An error occurs...
     */
  failed:
    *end = -1;
    eb_unlock(&appendix->lock);
    return error_code;
}


/*
 * Get the alternation text of the character number `character_number'.
 */
EB_Error_Code
eb_narrow_alt_character_text(appendix, character_number, text)
    EB_Appendix *appendix;
    int character_number;
    char *text;
{
    EB_Error_Code error_code;

    /*
     * Lock the appendix.
     */
    eb_lock(&appendix->lock);

    /*
     * Current subbook must have been set.
     */
    if (appendix->subbook_current == NULL) {
	error_code = EB_ERR_NO_CUR_APPSUB;
	goto failed;
    }

    /*
     * The narrow font must be exist in the current subbook.
     */
    if (appendix->subbook_current->narrow_page == 0) {
	error_code = EB_ERR_NO_SUCH_CHAR_TEXT;
	goto failed;
    }

    if (appendix->subbook_current->character_code == EB_CHARCODE_ISO8859_1) {
	error_code = eb_narrow_character_text_latin(appendix,
	    character_number, text);
    } else {
	error_code = eb_narrow_character_text_jis(appendix, character_number,
	    text);
    }
    if (error_code != EB_SUCCESS)
	goto failed;

    /*
     * Unlock the appendix.
     */
    eb_unlock(&appendix->lock);

    return EB_SUCCESS;

    /*
     * An error occurs...
     */
  failed:
    *text = '\0';
    eb_unlock(&appendix->lock);
    return error_code;
}


/*
 * Get the alternation text of the character number `character_number'.
 */
static EB_Error_Code
eb_narrow_character_text_jis(appendix, character_number, text)
    EB_Appendix *appendix;
    int character_number;
    char *text;
{
    EB_Error_Code error_code;
    int start = appendix->subbook_current->narrow_start;
    int end = appendix->subbook_current->narrow_end;
    int location;
    EB_Alternation_Cache *cachep;

    start = appendix->subbook_current->narrow_start;
    end = appendix->subbook_current->narrow_end;

    /*
     * Check for `character_number'.  Is it in a font?
     * This test works correctly even when the font doesn't exist in
     * the current subbook because `start' and `end' have set to -1
     * in the case.
     */
    if (character_number < start
	|| end < character_number
	|| (character_number & 0xff) < 0x21
	|| 0x7e < (character_number & 0xff)) {
	error_code = EB_ERR_NO_SUCH_CHAR_TEXT;
	goto failed;
    }

    /*
     * Calculate the location of alternation data.
     */
    location = (appendix->subbook_current->narrow_page - 1) * EB_SIZE_PAGE
	+ (((character_number >> 8) - (start >> 8)) * 0x5e
	    + (character_number & 0xff) - (start & 0xff))
	* (EB_MAX_ALTERNATION_TEXT_LENGTH + 1);

    /*
     * Check for the cache data.
     */
    cachep = appendix->narrow_cache + EB_HASH_ALT_CACHE(character_number);
    if (cachep->character_number == character_number) {
	memcpy(text, cachep->text, EB_MAX_ALTERNATION_TEXT_LENGTH + 1);
	goto succeeded;
    }

    /*
     * Read the alternation data.
     */
    if (eb_zlseek(&appendix->subbook_current->appendix_zip, 
	appendix->subbook_current->appendix_file, location, SEEK_SET) < 0) {
	error_code = EB_ERR_FAIL_SEEK_APP;
	goto failed;
    }
    cachep->character_number = -1;
    if (eb_zread(&appendix->subbook_current->appendix_zip, 
	appendix->subbook_current->appendix_file, cachep->text, 
	EB_MAX_ALTERNATION_TEXT_LENGTH + 1)
	!= EB_MAX_ALTERNATION_TEXT_LENGTH + 1) {
	error_code = EB_ERR_FAIL_READ_APP;
	goto failed;
    }

    /*
     * Update cache data.
     */
    memcpy(text, cachep->text, EB_MAX_ALTERNATION_TEXT_LENGTH + 1);
    cachep->text[EB_MAX_ALTERNATION_TEXT_LENGTH] = '\0';
    cachep->character_number = character_number;

  succeeded:
    return EB_SUCCESS;

    /*
     * An error occurs...
     */
  failed:
    *text = '\0';
    return error_code;
}


/*
 * Get the alternation text of the character number `character_number'.
 */
static EB_Error_Code
eb_narrow_character_text_latin(appendix, character_number, text)
    EB_Appendix *appendix;
    int character_number;
    char *text;
{
    EB_Error_Code error_code;
    int start = appendix->subbook_current->narrow_start;
    int end = appendix->subbook_current->narrow_end;
    int location;
    EB_Alternation_Cache *cache_p;

    /*
     * Check for `character_number'.  Is it in a font?
     * This test works correctly even when the font doesn't exist in
     * the current subbook because `start' and `end' have set to -1
     * in the case.
     */
    if (character_number < start
	|| end < character_number
	|| (character_number & 0xff) < 0x01
	|| 0xfe < (character_number & 0xff)) {
	error_code = EB_ERR_NO_SUCH_CHAR_TEXT;
	goto failed;
    }

    /*
     * Calculate the location of alternation data.
     */
    location = (appendix->subbook_current->narrow_page - 1) * EB_SIZE_PAGE
	+ (((character_number >> 8) - (start >> 8)) * 0xfe
	    + (character_number & 0xff) - (start & 0xff))
	* (EB_MAX_ALTERNATION_TEXT_LENGTH + 1);

    /*
     * Check for the cache data.
     */
    cache_p = appendix->narrow_cache + EB_HASH_ALT_CACHE(character_number);
    if (cache_p->character_number == character_number) {
	memcpy(text, cache_p->text, EB_MAX_ALTERNATION_TEXT_LENGTH + 1);
	goto succeeded;
    }

    /*
     * Read the alternation data.
     */
    if (eb_zlseek(&appendix->subbook_current->appendix_zip, 
	appendix->subbook_current->appendix_file, location, SEEK_SET) < 0) {
	error_code = EB_ERR_FAIL_SEEK_APP;
	goto failed;
    }
    cache_p->character_number = -1;
    if (eb_zread(&appendix->subbook_current->appendix_zip, 
	appendix->subbook_current->appendix_file, cache_p->text, 
	EB_MAX_ALTERNATION_TEXT_LENGTH + 1)
	!= EB_MAX_ALTERNATION_TEXT_LENGTH + 1) {
	error_code = EB_ERR_FAIL_READ_APP;
	goto failed;
    }

    /*
     * Update cache data.
     */
    memcpy(text, cache_p->text, EB_MAX_ALTERNATION_TEXT_LENGTH + 1);
    cache_p->text[EB_MAX_ALTERNATION_TEXT_LENGTH] = '\0';
    cache_p->character_number = character_number;

  succeeded:
    return EB_SUCCESS;

    /*
     * An error occurs...
     */
  failed:
    *text = '\0';
    return error_code;
}


/*
 * Return next `n'th character number from `*character_number'.
 */
EB_Error_Code
eb_forward_narrow_alt_character(appendix, n, character_number)
    EB_Appendix *appendix;
    int n;
    int *character_number;
{
    EB_Error_Code error_code;
    int start;
    int end;
    int i;

    if (n < 0) {
	return eb_backward_narrow_alt_character(appendix, -n,
	    character_number);
    }

    /*
     * Lock the appendix.
     */
    eb_lock(&appendix->lock);

    /*
     * Current subbook must have been set.
     */
    if (appendix->subbook_current == NULL) {
	error_code = EB_ERR_NO_CUR_APPSUB;
	goto failed;
    }

    /*
     * The narrow font must be exist in the current subbook.
     */
    if (appendix->subbook_current->narrow_page == 0) {
	error_code = EB_ERR_NO_SUCH_CHAR_TEXT;
	goto failed;
    }

    start = appendix->subbook_current->narrow_start;
    end = appendix->subbook_current->narrow_end;

    if (appendix->subbook_current->character_code == EB_CHARCODE_ISO8859_1) {
	/*
	 * Check for `*character_number'. (ISO 8859 1)
	 */
	if (*character_number < start
	    || end < *character_number
	    || (*character_number & 0xff) < 0x01
	    || 0xfe < (*character_number & 0xff)) {
	    error_code = EB_ERR_NO_SUCH_CHAR_TEXT;
	    goto failed;
	}

	/*
	 * Get character number. (ISO 8859 1)
	 */
	for (i = 0; i < n; i++) {
	    if (0xfe <= (*character_number & 0xff))
		*character_number += 3;
	    else
		*character_number += 1;
	    if (end < *character_number) {
		error_code = EB_ERR_NO_SUCH_CHAR_TEXT;
		goto failed;
	    }
	}
    } else {
	/*
	 * Check for `*character_number'. (JIS X 0208)
	 */
	if (*character_number < start
	    || end < *character_number
	    || (*character_number & 0xff) < 0x21
	    || 0x7e < (*character_number & 0xff)) {
	    error_code = EB_ERR_NO_SUCH_CHAR_TEXT;
	    goto failed;
	}

	/*
	 * Get character number. (JIS X 0208)
	 */
	for (i = 0; i < n; i++) {
	    if (0x7e <= (*character_number & 0xff))
		*character_number += 0xa3;
	    else
		*character_number += 1;
	    if (end < *character_number) {
		error_code = EB_ERR_NO_SUCH_CHAR_TEXT;
		goto failed;
	    }
	}
    }

    /*
     * Unlock the appendix.
     */
    eb_unlock(&appendix->lock);

    return EB_SUCCESS;

    /*
     * An error occurs...
     */
  failed:
    *character_number = -1;
    eb_unlock(&appendix->lock);
    return error_code;
}


/*
 * Return previous `n'th character number from `*character_number'.
 */
EB_Error_Code
eb_backward_narrow_alt_character(appendix, n, character_number)
    EB_Appendix *appendix;
    int n;
    int *character_number;
{
    EB_Error_Code error_code;
    int start;
    int end;
    int i;

    if (n < 0) {
	return eb_forward_narrow_alt_character(appendix, -n, character_number);
    }

    /*
     * Lock the appendix.
     */
    eb_lock(&appendix->lock);

    /*
     * Current subbook must have been set.
     */
    if (appendix->subbook_current == NULL) {
	error_code = EB_ERR_NO_CUR_APPSUB;
	goto failed;
    }

    /*
     * The narrow font must be exist in the current subbook.
     */
    if (appendix->subbook_current->narrow_page == 0) {
	error_code = EB_ERR_NO_CUR_FONT;
	goto failed;
    }

    start = appendix->subbook_current->narrow_start;
    end = appendix->subbook_current->narrow_end;

    if (appendix->subbook_current->character_code == EB_CHARCODE_ISO8859_1) {
	/*
	 * Check for `*character_number'. (ISO 8859 1)
	 */
	if (*character_number < start
	    || end < *character_number
	    || (*character_number & 0xff) < 0x01
	    || 0xfe < (*character_number & 0xff)) {
	    error_code = EB_ERR_NO_SUCH_CHAR_TEXT;
	    goto failed;
	}

	/*
	 * Get character number. (ISO 8859 1)
	 */
	for (i = 0; i < n; i++) {
	    if ((*character_number & 0xff) <= 0x01)
		*character_number -= 3;
	    else
		*character_number -= 1;
	    if (*character_number < start) {
		error_code = EB_ERR_NO_SUCH_CHAR_TEXT;
		goto failed;
	    }
	}
    } else {
	/*
	 * Check for `*character_number'. (JIS X 0208)
	 */
	if (*character_number < start || end < *character_number || (*character_number & 0xff) < 0x21
	    || 0x7e < (*character_number & 0xff)) {
	    error_code = EB_ERR_NO_SUCH_CHAR_TEXT;
	    goto failed;
	}

	/*
	 * Get character number. (JIS X 0208)
	 */
	for (i = 0; i < n; i++) {
	    if ((*character_number & 0xff) <= 0x21)
		*character_number -= 0xa3;
	    else
		*character_number -= 1;
	    if (*character_number < start) {
		error_code = EB_ERR_NO_SUCH_CHAR_TEXT;
		goto failed;
	    }
	}
    }

    /*
     * Unlock the appendix.
     */
    eb_unlock(&appendix->lock);

    return EB_SUCCESS;

    /*
     * An error occurs...
     */
  failed:
    *character_number = -1;
    eb_unlock(&appendix->lock);
    return error_code;
}


