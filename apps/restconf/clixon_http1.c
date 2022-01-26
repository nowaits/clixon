/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2020-2022 Olof Hagsand and Rubicon Communications, LLC(Netgate)

  This file is part of CLIXON.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  Alternatively, the contents of this file may be used under the terms of
  the GNU General Public License Version 3 or later (the "GPL"),
  in which case the provisions of the GPL are applicable instead
  of those above. If you wish to allow use of your version of this file only
  under the terms of the GPL, and not to allow others to
  use your version of this file under the terms of Apache License version 2, 
  indicate your decision by deleting the provisions above and replace them with
  the  notice and other provisions required by the GPL. If you do not delete
  the provisions above, a recipient may use your version of this file under
  the terms of any one of the Apache License version 2 or the GPL.

  ***** END LICENSE BLOCK *****

 * HTTP/1.1 parser according to RFC 7230
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <syslog.h>
#include <errno.h>
#include <openssl/ssl.h>

#ifdef HAVE_LIBNGHTTP2
#include <nghttp2/nghttp2.h>
#endif

/* cligen */
#include <cligen/cligen.h>

/* clixon */
#include <clixon/clixon.h>

#include "restconf_lib.h"
#include "restconf_native.h"
#include "clixon_http1_parse.h"

/* Size of xml read buffer */
#define BUFLEN 1024  

static int 
_http1_parse(clicon_handle  h,
	     restconf_conn *rc,
	     char          *str,
	     const char    *filename)
{
    int               retval = -1;
    clixon_http1_yacc hy = {0,};
    char             *ptr;
    size_t            sz;
    
    clicon_debug(2, "%s", __FUNCTION__);
    if (strlen(str) == 0)
	goto ok;
    hy.hy_parse_string = str;
    hy.hy_name = filename;
    hy.hy_h = h;
    hy.hy_rc = rc;
    hy.hy_linenum = 1;
    if (http1_scan_init(&hy) < 0)
	goto done;
    if (http1_parse_init(&hy) < 0)
	goto done;
    ptr = clixon_http1_parsetext;
    if (clixon_http1_parseparse(&hy) != 0) { /* yacc returns 1 on error */
	if (filename)
	    clicon_log(LOG_NOTICE, "HTTP1 error: on line %d in %s", hy.hy_linenum, filename);
	else
	    clicon_log(LOG_NOTICE, "HTTP1 error: on line %d", hy.hy_linenum);
	if (clicon_errno == 0)
	    clicon_err(OE_RESTCONF, 0, "HTTP1 parser error with no error code (should not happen)");
	goto done;
    }
    if (0){
	sz =  (clixon_http1_parsetext - ptr) + strlen(clixon_http1_parsetext);
	fprintf(stderr,"%s %p diff:%ld %ld\n", __FUNCTION__, 
		clixon_http1_parsetext,
		sz,
		strlen(ptr)
		);
    }
    http1_parse_exit(&hy);
    http1_scan_exit(&hy);
 ok:
    retval = 0;
 done:
    return retval;
}

/*! Read an XML definition from file and parse it into a parse-tree, advanced API
 *
 * @param[in]     fd    A file descriptor containing the XML file (as ASCII characters)
 * @param[in]     yb    How to bind yang to XML top-level when parsing
 * @param[in]     yspec Yang specification (only if bind is TOP or CONFIG)
 * @param[in,out] xt    Pointer to XML parse tree. If empty, create.
 * @param[out]    xerr  Pointer to XML error tree, if retval is 0
 * @retval        1     Parse OK and all yang assignment made
 * @retval        0     Parse OK but yang assigment not made (or only partial) and xerr set
 * @retval       -1     Error with clicon_err called. Includes parse error
 *
 * @code
 *  cxobj *xt = NULL;
 *  cxobj *xerr = NULL;
 *  FILE  *f;
 *  if ((f = fopen(filename, "r")) == NULL)
 *    err;
 *  if ((ret = clixon_xml_parse_file(f, YB_MODULE, yspec, &xt, &xerr)) < 0)
 *    err;
 *  xml_free(xt);
 * @endcode
 * @see clixon_xml_parse_string
 * @see clixon_json_parse_file
 * @note, If xt empty, a top-level symbol will be added so that <tree../> will be:  <top><tree.../></tree></top>
 * @note May block on file I/O
 */
int
clixon_http1_parse_file(clicon_handle  h,
			restconf_conn *rc,
			FILE          *f,
			const char    *filename)
{
    int   retval = -1;
    int   ret;
    char  ch;
    char *buf = NULL;
    char *ptr;
    int   buflen = BUFLEN; /* start size */
    int   len = 0;
    int   oldbuflen;

    clicon_debug(1, "%s %s", __FUNCTION__, filename);
    if (f == NULL){
	clicon_err(OE_RESTCONF, EINVAL, "f is NULL");
	goto done;
    }
    if ((buf = malloc(buflen)) == NULL){
	clicon_err(OE_XML, errno, "malloc");
	goto done;
    }
    memset(buf, 0, buflen);
    ptr = buf;
    while (1){
	if ((ret = fread(&ch, 1, 1, f)) < 0){
	    clicon_err(OE_XML, errno, "read");
	    break;
	}
	if (ret != 0){
	    buf[len++] = ch;
	}
	if (ret == 0) { /* buffer read */
	    if (_http1_parse(h, rc, ptr, filename) < 0)
		goto done;
	    break;
	}
	if (len >= buflen-1){ /* Space: one for the null character */
	    oldbuflen = buflen;
	    buflen *= 2;
	    if ((buf = realloc(buf, buflen)) == NULL){
		clicon_err(OE_XML, errno, "realloc");
		goto done;
	    }
	    memset(buf+oldbuflen, 0, buflen-oldbuflen);
	    ptr = buf;
	}
    } /* while */
    retval = 0;
 done:
    if (buf)
	free(buf);
    return retval;
}

int 
clixon_http1_parse_string(clicon_handle  h,
			  restconf_conn *rc,
			  char          *str)
{
    return _http1_parse(h, rc, str, "http1-parse");
}

/*! Convert buffer to null-terminated string
 * I dont know how to do this without copying, OR 
 * input flex with a non-null terminated string
 */
int 
clixon_http1_parse_buf(clicon_handle  h,
		       restconf_conn *rc,
		       char          *buf,
		       size_t         n)
{
    char *str = NULL;

    if ((str = malloc(n+1)) == NULL){
	clicon_err(OE_RESTCONF, errno, "malloc");
	return -1;
    }
    memcpy(str, buf, n);
    str[n] = '\0';
    return _http1_parse(h, rc, str, "http1-parse");
}

/*!
 * @param[in]  h    Clixon handle
 * @param[in]  rc   Clixon request connect pointer
 */
int
restconf_http1_path_root(clicon_handle  h,
			 restconf_conn *rc)
{
    int                   retval = -1;
    restconf_stream_data *sd;

    clicon_debug(1, "------------");
    if ((sd = restconf_stream_find(rc, 0)) == NULL){
	clicon_err(OE_RESTCONF, EINVAL, "No stream_data");
	goto done;
    }
    retval = 0;
 done:
    clicon_debug(1, "%s %d", __FUNCTION__, retval);
    return retval;
}