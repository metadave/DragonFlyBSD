/*-
 * Copyright (c) 1988, 1989, 1990, 1993
 *	The Regents of the University of California.  All rights reserved.
 * Copyright (c) 1989 by Berkeley Softworks
 * All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Adam de Boor.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $DragonFly: src/usr.bin/make/parse.h,v 1.4 2005/02/04 06:45:16 okumoto Exp $
 */

#ifndef parse_h_470eeb9a
#define	parse_h_470eeb9a

#include <stdio.h>

#include "sprite.h"

struct GNode;
struct Lst;

/*
 * Definitions for handling #include specifications
 */
typedef struct {
    char *str;
    char *ptr;
} PTR;
typedef struct IFile {
    char            *fname;	    /* name of previous file */
    int             lineno;	    /* saved line number */
    FILE	    *F;		    /* the open stream */
    PTR		    *p;	    	    /* the char pointer */
} IFile;

void Parse_Error(int, const char *, ...);
Boolean Parse_AnyExport(void);
Boolean Parse_IsVar(char *);
void Parse_DoVar(char *, struct GNode *);
void Parse_AddIncludeDir(char *);
void Parse_File(char *, FILE *);
void Parse_Init(void);
void Parse_FromString(char *, int);
void Parse_MainName(struct Lst *);

#endif /* parse_h_470eeb9a */
