/*
Copyright (c) 1999.  The Regents of the University of California
(Regents). All Rights Reserved.

Permission to use, copy, modify, and distribute this software and its
documentation for educational, research, and not-for-profit purposes,
without fee and without a signed licensing agreement, is hereby granted,
provided that the above copyright notice, this paragraph and the
following two paragraphs appear in all copies, modifications, and distributions.
Contact The Office of Technology Licensing, UC Berkeley, 2150 Shattuck
Avenue, Suite 510, Berkeley, CA 94720-1620, (510) 643-7201, for commercial
licensing opportunities.

Written by Matt Wright, The Center for New Music and Audio Technologies,
University of California, Berkeley.  Based on sample code from David Zicarelli.

     IN NO EVENT SHALL REGENTS BE LIABLE TO ANY PARTY FOR DIRECT, INDIRECT,
     SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING LOST
     PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS
     DOCUMENTATION, EVEN IF REGENTS HAS BEEN ADVISED OF THE POSSIBILITY OF
     SUCH DAMAGE.

     REGENTS SPECIFICALLY DISCLAIMS ANY WARRANTIES, INCLUDING, BUT NOT
     LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
     FOR A PARTICULAR PURPOSE. THE SOFTWARE AND ACCOMPANYING
     DOCUMENTATION, IF ANY, PROVIDED HEREUNDER IS PROVIDED "AS IS".
     REGENTS HAS NO OBLIGATION TO PROVIDE MAINTENANCE, SUPPORT, UPDATES,
     ENHANCEMENTS, OR MODIFICATIONS.

*/

/* --1/4/99 SDIF-buffer.c -- the SDIF-buffer object -- 

  10/14/99 - Updated by Matt to use the new SDIF library 
  11/16/00 - Version 0.4 write to SDIF files
  11/21/00 - 0.4.1 default streamID is 1; added change-streamID message
  10/08/02 - 0.5: Compiles with CW7
  11/21/02 - 0.6: Has change-frametype message
    

*/


#define SDIF_BUFFER_VERSION "0.6"

/* the required include files */
#include <Navigation.h>
#include <FSp_fopen.h>

#include "ext.h"
#include <limits.h>
#include <string.h>

/* Undo ext.h's macro versions of some of stdio.h: */
#undef fopen
#undef fclose
#undef fprintf
#undef fscanf
#undef fseek
#undef sprintf
#undef sscanf

#include "SDIF-buffer.h"

/* #include <assert.h> */

#define assert(x) if (!(x)) { ouchstring("Assertion failed: %s, file %s, line %i\n", \
							             #x, __FILE__, __LINE__); } else {}


/* This struct contains the "private" data for an SDIF-buffer: */
typedef struct _SDIFbuffer_private {
	SDIFmem_Frame head, tail;	/* Pointers to ends of doubly-linked list */
	SDIFBuffer *next;	/* For linked list of all buffers, for name lookup */
	int debug;			/* 0 or non-zero for debug mode. */
} SDIFBufferPrivate;


/* globals */
void *SDIFbuffer_class;
Symbol *ps_SDIFbuffer, *ps_SDIF_buffer_lookup, *ps_emptysymbol;
SDIFBuffer *AllTheBuffers;	/* A linked list of all the buffers */


/* Different modes of choosing which stream to read from an SDIF file */
typedef enum {
	STREAM_NUMBER,
	ONLY_STREAM,
	FIRST_STREAM,
	ONLY_STREAM_TYPE,
	FIRST_STREAM_TYPE
} WhichStreamMode;


/* prototypes for my functions */
void *my_getbytes(int numBytes);
void my_freebytes(void *bytes, int size);
void *SDIFbuffer_new(Symbol *name, Symbol *filename);
void SDIFbuffer_free(SDIFBuffer *x);
void SDIFbuffer_clear(SDIFBuffer *x);
void SDIFbuffer_doclear(SDIFBuffer *x);
static FILE *OpenSDIFFile(char *filename);
void ReadStream(SDIFBuffer *x, char *filename, WhichStreamMode mode, long arg);
void SDIFbuffer_readstreamnumber(SDIFBuffer *x, Symbol *fileName, long streamID);
void SDIFbuffer_streamlist(SDIFBuffer *, Symbol *, int argc, Atom *argv);
void one_streamlist(Symbol *fileName);
void SDIFbuffer_framelist(SDIFBuffer *, Symbol *, int argc, Atom *argv);
void one_framelist(Symbol *fileName);
static void do_streamlist(FILE *f, char *name, int showframes);
void SDIFbuffer_print(SDIFBuffer *x);
void SDIFbuffer_NAVcrap(SDIFBuffer *x);
void PrintOneFrame(SDIFmem_Frame f);
void PrintFrameHeader(SDIF_FrameHeader *fh);
void PrintMatrixHeader(SDIF_MatrixHeader *mh);
static SDIFmem_Frame ListSearch(SDIFBuffer *x, sdif_float64 time, long direction);
static int ListInsert(SDIFmem_Frame f, struct _SDIFbuffer *buf);
SDIFBuffer *MySDIFBufferLookupFunction(t_symbol *name);
void AddNewBuffer(SDIFBuffer *b);
void DeleteBuffer(SDIFBuffer *goner);
void PrintAllTheBuffers(void);
void SDIFbuffer_changeStreamID(SDIFBuffer *x, long newStreamID);
void SDIFbuffer_changeFrameType(SDIFBuffer *x, t_symbol *newFrameType);
void SDIFbuffer_debug(SDIFBuffer *x, long debugMode);



void SDIFbuffer_writefile(SDIFBuffer *x, Symbol *fileName);


void *my_getbytes(int numBytes) {
	if (numBytes > SHRT_MAX) {
			return 0;
	}
	return (void *) getbytes((short) numBytes);
}

void my_freebytes(void *bytes, int size) {
	freebytes(bytes, (short) size);
}

void main(fptr *fp) {
	SDIFresult r;
	
	post("SDIF-buffer version " SDIF_BUFFER_VERSION " by Matt Wright and Tim Madden");
	post("Copyright � 1999,2000,01,02 Regents of the University of California.");
	
	/* tell Max about my class. The cast to short is important for 68K */
	setup((t_messlist **)&SDIFbuffer_class, (method)SDIFbuffer_new, (method)SDIFbuffer_free,
			(short)sizeof(SDIFBuffer), 0L, A_SYM, A_DEFSYM, 0);
	
	/* bind my methods to symbols */
	addmess((method)SDIFbuffer_readstreamnumber, "read-stream-number", A_SYM, A_LONG, 0);
	addmess((method)SDIFbuffer_streamlist, "streamlist", A_GIMME, 0);
	addmess((method)SDIFbuffer_framelist, "framelist", A_GIMME, 0);
	addmess((method)SDIFbuffer_print, "print", 0);
	addmess((method)SDIFbuffer_clear, "clear", 0);
	addmess((method)SDIFbuffer_NAVcrap, "NAVcrap", 0);
	addmess((method)PrintAllTheBuffers, "printall", 0);
	addmess((method)SDIFbuffer_writefile, "write", A_SYM, 0);
	addmess((method)SDIFbuffer_changeStreamID, "change-streamID", A_LONG, 0);
	addmess((method)SDIFbuffer_changeFrameType, "change-frametype", A_SYM, 0);
	addmess((method)SDIFbuffer_debug, "debug", A_LONG, 0);



	/* list object in the new object list */
	finder_addclass("Data","SDIF-buffer");
	
	if (r = SDIF_Init()) {
		ouchstring("Couldn't initialize SDIF library! %s", SDIF_GetErrorString(r));
	}
	
	if (r = SDIFmem_Init(my_getbytes, my_freebytes)) {
		ouchstring("Couldn't initialize SDIF memory utilities! %s", SDIF_GetErrorString(r));
	}
	
	if (!NavServicesAvailable()) {
		post("� SDIF-buffer: navigation services are not available.");
		post("Opening a dialog box will probably fail.");
	} else {	
		OSErr err = NavLoad();
		if (err != noErr) {
			post("� SDIF-buffer: NavLoad() gave error %ld", (long) err);
			post("Opening a dialog box will probably fail.");
		}
	}
	
	ps_SDIFbuffer = gensym("SDIF-buffer");
	ps_SDIF_buffer_lookup = gensym("##SDIF-buffer-lookup");
	ps_emptysymbol = gensym("");
	
	if (ps_SDIF_buffer_lookup->s_thing != 0) {
		post("� SDIF-buffer: warning: SDIF-buffer-lookup s_thing not zero.");
	}
	ps_SDIF_buffer_lookup->s_thing = (void *) MySDIFBufferLookupFunction;
}

void *SDIFbuffer_new(Symbol *name, Symbol *filename) {
	SDIFBuffer *x;
	SDIFBufferPrivate *privateStuff;

	

	if (MySDIFBufferLookupFunction(name) != 0) {
		post("� %s is already an SDIF-buffer!", name->s_name);
		return 0;
	}
	
	x = newobject(SDIFbuffer_class);
	x->s_myname = name;
	x->FrameLookup = ListSearch;
	x->FrameInsert = ListInsert;
	x->internal = getbytes((short) sizeof(SDIFBufferPrivate));
	privateStuff = (SDIFBufferPrivate *) x->internal;
	privateStuff->head = 0;	
	privateStuff->tail = 0;	
	privateStuff->debug = 0;	

	SDIFbuffer_doclear(x);
	AddNewBuffer(x);
	
	if (filename != ps_emptysymbol) {
		post("Need to load sdif file %s", filename->s_name);
	}
	
	
	return (x);
}

static void SDIFbuffer_freeWholeStream(SDIFBuffer *x) {
	SDIFmem_Frame f, next;
	SDIFBufferPrivate *privateStuff;

	privateStuff = (SDIFBufferPrivate *) x->internal;	
	f = privateStuff->head;
	
	while (f != NULL) {
		next = f->next;
		SDIFmem_FreeFrame(f);
		f = next;
	}
}

void SDIFbuffer_free(SDIFBuffer *x) {
	
	
	SDIFbuffer_freeWholeStream(x);
	DeleteBuffer(x);
	
}

void SDIFbuffer_doclear(SDIFBuffer *x) {
	SDIFBufferPrivate *privateStuff;

	SDIFbuffer_freeWholeStream(x);
	x->fileName = 0;
	x->streamID = 1;
	SDIF_Copy4Bytes(x->frameType, "----");
	x->min_time = 0.0;
	x->max_time = 0.0;

	privateStuff = (SDIFBufferPrivate *) x->internal;
	privateStuff->head = 0;	
	privateStuff->tail = 0;	
}	

void SDIFbuffer_clear(SDIFBuffer *x) {
	
	SDIFbuffer_doclear(x);
		
}

void SDIFbuffer_debug(SDIFBuffer *x, long debugMode) {
	SDIFBufferPrivate *privateStuff = (SDIFBufferPrivate *) x->internal;
	
	privateStuff->debug = debugMode;	
}


static FILE *OpenSDIFFile(char *filename) {
#ifdef ALWAYS_WANT_TO_LOOK_IN_MAX_FOLDER
	return SDIF_OpenRead(filename);
#else
	SDIFresult r;
	short vRefNum;
	FSSpec spec;
	OSErr err;
	FILE *f;
	
	if (locatefiletype(filename, &vRefNum, 0L, 0L) != 0) {
		post("� SDIF-buffer: couldn't locate alleged SDIF file %s", filename);
		return NULL;
	}
	
	// Convert to an FSSpec
	err = FSMakeFSSpec (vRefNum, 0, CtoPstr(filename), &spec);
	PtoCstr((unsigned char *) filename);
	if (err != noErr) {
		if (err == nsvErr) {
			post("� SDIF-buffer: locatefiletype returned a volume ref number that doesn't exist!");
		} else if (err == bdNamErr) {
			post("� SDIF-buffer: FSMakeFSSpec thought your filename sucked.");
		} else {
			post("� SDIF-buffer: FSMakeFSSpec returned %d", err);
		}
		return NULL;
	}
	
	f = FSp_fopen (&spec, "rb");
	if (f == NULL) {
		post("� SDIF-buffer: FSp_fopen returned NULL!");
	} else {
		if (r = SDIF_BeginRead(f)) {
			int ferrno;
			post("� SDIF-buffer: error reading SDIF file %s:", filename);
			post("  %s", SDIF_GetErrorString(r));
			
			ferrno = ferror(f);
			post("  ferror() returned %ld:", ferrno);
			post("      %s", strerror(ferrno));
			
			fclose(f);
			return NULL;
		}
	} 
	return f;
#endif
}


void SDIFbuffer_changeStreamID(SDIFBuffer *x, long newStreamID) {
	SDIFBufferPrivate *privateStuff;
	SDIFmem_Frame f;
		
	privateStuff = (SDIFBufferPrivate *) x->internal;
	for (f = privateStuff->head; f != 0; f = f->next) {
		f->header.streamID = newStreamID;	
	}
	
	x->streamID = newStreamID;
}



void SDIFbuffer_changeFrameType(SDIFBuffer *x, t_symbol *newFrameType) {
	SDIFBufferPrivate *privateStuff;
	SDIFmem_Frame f;
	char *type = newFrameType->s_name;
	
	if (type[0] == '\0' || type[1] == '\0' || type[2] == '\0' || type[3] == '\0' || type[4] != '\0') {
		post("� SDIF-buffer: change-frametype: illegal type \"%s\" is not 4 characters.", type);
		return;
	}
	
	privateStuff = (SDIFBufferPrivate *) x->internal;
	for (f = privateStuff->head; f != 0; f = f->next) {
		SDIF_Copy4Bytes(f->header.frameType, type);	
	}
	
	SDIF_Copy4Bytes(x->frameType, type);	
}




/* Methods I want:

	read-stream-number
	read_only_stream
	read_first_stream
	read_only_stream_type 1TRC
	read_first_stream_type 1TRC
*/



void SDIFbuffer_readstreamnumber(SDIFBuffer *x, Symbol *fileNameSymbol, long streamID) {	
	

	// post("SDIFbuffer_readstreamnumber: file %s, stream %ld", fileName->s_name, streamID);
	ReadStream(x, fileNameSymbol->s_name, STREAM_NUMBER, streamID);

		
}


void ReadStream(SDIFBuffer *x, char *filename, WhichStreamMode mode, long arg) {	
	FILE *f;
	SDIF_FrameHeader fh;
	SDIFmem_Frame previous, current, first;
	SDIFBufferPrivate *privateStuff;
	SDIFresult r;
	sdif_int32 streamID;
	
	
	privateStuff = (SDIFBufferPrivate *) x->internal;

	if (mode == STREAM_NUMBER) {
		streamID = arg;
	} else {
		post("SDIF-buffer: ReadStream: unrecognized mode %ld", mode);
		return;
	}	
	
	if (privateStuff->debug) {
		post(" SDIF-buffer debug: trying to read stream %d from file \"%s\"", streamID, filename);
	}

			
	f = OpenSDIFFile(filename);
	if (f == NULL) {
		/* OpenSDIFFile already printed an error message */
		return;
	} else {
		/* If there was a stream in this buffer before, flush it. */
		SDIFbuffer_doclear(x);
	}

	if (privateStuff->debug) {
		post(" SDIF-buffer debug: opened \"%s\" for reading", filename);
	}


	first = current = previous = (SDIFmem_Frame) NULL;
	
	while ((r = SDIF_ReadFrameHeader(&fh, f)) == 0) {
		if (privateStuff->debug) {
			post(" SDIF-buffer debug: read frame header: stream %d, time %g, size %d",
				 fh.streamID, fh.time, fh.size);
		}
		if (fh.streamID == streamID) {
			/* We want this one */
			r = SDIFmem_ReadFrameContents(&fh, f, &current);
			if (r) {
				post("� SDIF-buffer: error reading frame from SDIF file %s:", filename);
				post("  %s", SDIF_GetErrorString(r));
				SDIF_CloseRead(f);
				return;
			}
			
			if (privateStuff->debug) {
				post(" SDIF-buffer debug: read contents of frame", fh.streamID, fh.time);
			}

			current->prev = previous;
			current->next = NULL;
			
			if (first == NULL) {
				first = current;
			} else {
				previous->next = current;
			}
			previous = current;
		} else {
			if (r = SDIF_SkipFrame(&fh, f)) {
				post("� SDIF-buffer: error skipping frame in SDIF file %s:", filename);
				post("  %s", SDIF_GetErrorString(r));
				SDIF_CloseRead(f);
				return;
			}
			if (privateStuff->debug) {
				post(" SDIF-buffer debug: skipped contents of frame", fh.streamID, fh.time);
			}
			
		}
	}
	

	if (r != ESDIF_END_OF_DATA) {
		post("� SDIF-buffer: error reading SDIF file %s:", filename);
		post("  %s", SDIF_GetErrorString(r));
		SDIF_CloseRead(f);
		return;
	}
	
	if (r = SDIF_CloseRead(f)) {
		post("� SDIF-buffer: error closing SDIF file %s:", filename);
		post(" %s", SDIF_GetErrorString(r));
	}
		
	if (first == NULL) {
		post("� SDIFbuffer_readstreamnumber: No frames found with StreamID %ld!",
			 streamID);
		return;
	}
	
	/* Now update all the buffer's state according to the newly read data */
	privateStuff->head = first;
	privateStuff->tail = current;
	x->fileName = filename;
	x->streamID = first->header.streamID;
	SDIF_Copy4Bytes(x->frameType, first->header.frameType);
	x->min_time = first->header.time;
	x->max_time = current->header.time;
}





void SDIFbuffer_streamlist(SDIFBuffer *, Symbol *, int argc, Atom *argv) {	
	int i;
	
	

	for (i = 0; i < argc; ++i) {
		if (argv[i].a_type != A_SYM) {
			post("SDIFbuffer_streamlist: ignoring numeric argument");
		} else {
			one_streamlist(argv[i].a_w.w_sym);
		}
	}

		
}


void one_streamlist(Symbol *fileName) {	
	FILE *f;
	SDIFresult r;

	post("SDIFbuffer_streamlist for file %s", fileName->s_name);
	f = OpenSDIFFile(fileName->s_name);
	if (f == NULL) {
		/* OpenSDIFFile already printed an error message */
		return;
	} 

	do_streamlist(f, fileName->s_name, 0);
	if ((r = SDIF_CloseRead(f)) != ESDIF_SUCCESS) {
		post("SDIF-buffer: error closing SDIF file %s:", fileName->s_name);
		post("%s", SDIF_GetErrorString(r));
	}
}



void SDIFbuffer_framelist(SDIFBuffer *, Symbol *, int argc, Atom *argv) {	
	int i;
	
	

	for (i = 0; i < argc; ++i) {
		if (argv[i].a_type != A_SYM) {
			post("SDIFbuffer_framelist: ignoring numeric argument");
		} else {
			one_framelist(argv[i].a_w.w_sym);
		}
	}

		
}



void one_framelist(Symbol *fileName) {	
	FILE *f;
	SDIFresult r;

	post("SDIFbuffer_framelist for file %s", fileName->s_name);
	f = OpenSDIFFile(fileName->s_name);
	if (f == NULL) {
		/* OpenSDIFFile already printed an error message */
		return;
	} 

	do_streamlist(f, fileName->s_name, 1);
	if (r = SDIF_CloseRead(f)) {
		post("SDIF-buffer: error closing SDIF file %s:", fileName->s_name);
		post("%s", SDIF_GetErrorString(r));
	}

}


#define MAX_STREAMS 200  // Most streams any file should have

static void do_streamlist(FILE *f, char *name, int showframes) {	
	SDIF_FrameHeader fh;
	int i;
	SDIFresult r;
	
	struct {
		sdif_int32 streamID[MAX_STREAMS];
		char frameType[MAX_STREAMS][4];
		int n;
	} streamsSeen;

	streamsSeen.n = 0;
	
	while ((r = SDIF_ReadFrameHeader(&fh, f)) == ESDIF_SUCCESS) {
		for (i = 0; i < streamsSeen.n; ++i) {
			if (streamsSeen.streamID[i] == fh.streamID) {
				// Already saw this stream, so just make sure type is OK
				if (!SDIF_Char4Eq(fh.frameType, streamsSeen.frameType[i])) {
					post("� streamlist: Warning: First frame for stream %ld", fh.streamID);
					post("� had type %c%c%c%c, but frame at time %g has type %c%c%c%c",
						 streamsSeen.frameType[i][0], streamsSeen.frameType[i][1],
						 streamsSeen.frameType[i][2], streamsSeen.frameType[i][3],
						 fh.time, fh.frameType[0], fh.frameType[1], fh.frameType[2],
						 fh.frameType[3]);
				}
				goto skip;
			}
		}
		post("  Stream ID %ld: frame type %c%c%c%c, starts at time %g",
			 fh.streamID, fh.frameType[0], fh.frameType[1], fh.frameType[2], fh.frameType[3],
			 fh.time);
			 
		if (streamsSeen.n >= MAX_STREAMS) {
			post(" � streamlist: error: SDIF file has more than %ld streams!", MAX_STREAMS);
		} else {
			streamsSeen.streamID[streamsSeen.n] = fh.streamID;
			SDIF_Copy4Bytes(streamsSeen.frameType[streamsSeen.n], fh.frameType);
			++(streamsSeen.n);
		}
		
		skip:

		if (showframes) {
			post("    Frame type %c%c%c%c, size %d, time %f, StreamID %d, %d matrices",
				 fh.frameType[0], fh.frameType[1], fh.frameType[2], fh.frameType[3],
				 fh.size, (float) fh.time, fh.streamID, fh.matrixCount);
		}

		if (r = SDIF_SkipFrame(&fh, f)) {
			post("SDIF-buffer: error skipping frame in SDIF file %s:", name);
			post("%s", SDIF_GetErrorString(r));
			return;
		}
	}

	if (r != ESDIF_END_OF_DATA) {
		post("SDIF-buffer: error reading SDIF file %s:", name);
		post("%s", SDIF_GetErrorString(r));
	}
}	

static SDIFmem_Frame ListSearch(SDIFBuffer *x, sdif_float64 time, long direction) {
	/// XXX: This belongs in SDIFmem.c
	SDIFmem_Frame f;
	SDIFBufferPrivate *privateStuff;

	// post("* ListSearch(SDIF-buffer %s, time %f, dir %d)", x->s_myname->s_name, (float) time, direction);
	
	privateStuff = (SDIFBufferPrivate *) x->internal;	
	f = privateStuff->head;
	
	if (f == 0) return 0;
	if (time < f->header.time) {
		/* Requested time is before time of first frame */
		if (direction > 0) return f;
		return 0;
	}
	
	// Loop invariant: time >= f->header.time
	while (f != 0) {
		if (time == f->header.time) {
			return f;
		}
		if (f->next == 0) {
			/* Requested time is after time of last frame */
			if (direction < 0) return f;
			return 0;
		}
			
		if (time < f->next->header.time) {
			/* Requested time is between this frame and next frame */
			if (direction < 0) return f;
			if (direction > 0) return f->next;
			return 0;
		}
			
		f = f->next;
	}
	
	/* Should never reach here */
	return 0;
}

static SDIFmem_Frame OldListSearch(SDIFBuffer *x, sdif_float64 time) {
	SDIFmem_Frame f;
	SDIFBufferPrivate *privateStuff;

	privateStuff = (SDIFBufferPrivate *) x->internal;	
	f = privateStuff->head;
	
	if (f == 0) return 0;
	if (f->header.time > time) return 0;
	
	while (f != 0) {
		if (f->next == 0 || f->next->header.time > time) {
			return f;
		}
		f = f->next;
	}
	
	/* Should never reach here */
	return 0;
}


static int ListInsert(SDIFmem_Frame newf, struct _SDIFbuffer *x) {
	sdif_float64 time = newf->header.time;
	SDIFmem_Frame f;
	SDIFBufferPrivate *privateStuff;

	privateStuff = (SDIFBufferPrivate *) x->internal;	
	f = privateStuff->head;

	//post("** ListInsert frame %p into buffer %p", newf, x);
	
	if (f == 0) {
		// This is the only frame;
		// post("** ListInsert -- the only frame");
		newf->prev = newf->next = 0;
		privateStuff->head = privateStuff->tail = newf;
		x->min_time = x->max_time = newf->header.time;
		return 0;
	}
	
	if (time < f->header.time) {
		// This is the new first frame
		// post("** ListInsert -- the new first frame");

		newf->prev = NULL;
		newf->next = privateStuff->head;
		privateStuff->head->prev = newf;
		privateStuff->head = newf;
		
		x->min_time = newf->header.time;
		return 0;
	}
	
	// Loop invariant: time >= f->header.time
	while (f != 0) {
		if (time == f->header.time) {
			// There's already a frame at this exact time
			post("� SDIF-buffer %s: can't insert a frame at time %f; there's already a frame at that time.",
				 x->s_myname->s_name, time);
			return 1;
		}
		if (f->next == 0) {
			// This is the new last frame
			// post("** ListInsert -- the new last frame");
			newf->prev = privateStuff->tail;
			newf->next = NULL;
			privateStuff->tail->next = newf;
			privateStuff->tail = newf;
			x->max_time = newf->header.time;
			return 0;
		}
		
		if (time < f->next->header.time) {
			/* The new frame belongs between f and f->next. */
			SDIFmem_Frame before, after;
			
			// post("** ListInsert -- new frame belongs between f and f->next");
			before = f;
			after = f->next;
			
			newf->prev = before;
			newf->next = after;
			before->next = newf;
			after->prev = newf;
			return 0;
		}			
		f = f->next;
	}	

	/* Should never reach here */
	return 10;
}


void SDIFbuffer_print(SDIFBuffer *x) {
	SDIFBufferPrivate *privateStuff;
	SDIFmem_Frame f;
	int numFrames;

	
	
	post("SDIFBuffer %s: file \"%s\"", x->s_myname->s_name, x->fileName);
	post("   Stream ID %ld, Frame Type %c%c%c%c", x->streamID,
		x->frameType[0], x->frameType[1], x->frameType[2], x->frameType[3]);
	post("   Min time %g, Max time %g", x->min_time, x->max_time);
	
	privateStuff = (SDIFBufferPrivate *) x->internal;
	
	if (privateStuff->head == 0) {
		post("   No frames!");
	}
	
	for (f = privateStuff->head, numFrames=0; f != NULL; f = f->next, ++numFrames) {
		if (numFrames < 10) PrintOneFrame(f);
	}
	if (numFrames >= 10) post("  ... and so on, %d frames total.", numFrames);

		
}

void PrintOneFrame(SDIFmem_Frame f) {
	SDIFmem_Matrix m;
	if (f == 0) {
		post("PrintOneFrame: null SDIFmem_Frame pointer");
		return;
	}
	// post("SDIF frame at %p, prev is %p, next is %p", f, f->prev, f->next);
	PrintFrameHeader(&(f->header));
	
	for (m = f->matrices; m != 0; m=m->next) {
		PrintMatrixHeader(&(m->header));
	}
}

void PrintFrameHeader(SDIF_FrameHeader *fh) {
	post(" Frame header: type %c%c%c%c, size %ld, time %g, stream ID %ld, %ld matrices",
		 fh->frameType[0], fh->frameType[1], fh->frameType[2], fh->frameType[3], 
		 fh->size, fh->time, fh->streamID, fh->matrixCount);
}
	
void PrintMatrixHeader(SDIF_MatrixHeader *mh) {
	post("  Matrix header: type %c%c%c%c, data type %ld, %ld rows, %ld cols",
		 mh->matrixType[0], mh->matrixType[1], mh->matrixType[2], mh->matrixType[3],
		 mh->matrixDataType, mh->rowCount, mh->columnCount);
}



pascal void worldsLamestEventProc(NavEventCallbackMessage callBackSelector,
						NavCBRecPtr callBackParms,
						NavCallBackUserData callBackUD);
pascal void worldsLamestEventProc(NavEventCallbackMessage callBackSelector,
						NavCBRecPtr callBackParms,
						NavCallBackUserData)
{
	WindowPtr window = (WindowPtr)callBackParms->eventData.eventDataParms.event->message;
	switch (callBackSelector) {
		case kNavCBEvent:
		switch (callBackParms->eventData.eventDataParms.event->what) {
			case updateEvt:
			/* MyHandleUpdateEvent(window,
								(EventRecord*)callBackParms->eventData.event); */
			break;
		}
	break;
	}
}

void SDIFbuffer_NAVcrap(SDIFBuffer *x) {
	OSErr err;
	struct NavReplyRecord nrr;
	long numItems, i;
	AEKeyword theKeyword;
	char *keywordAsString;
	DescType typeIGot;
	FSSpec theSpec;
	Size sizeIGot;
	FILE *f;
	
	

	post("* about to NavGetFile");
	err = NavGetFile(NULL,  /* Default location is last folder opened */
			  		 &nrr, /* where answer will go */
			  		 NULL, /* Use default dialog options */
			  		 NewNavEventProc(&worldsLamestEventProc), /* So we can move+resize dialog box*/
					 NULL, /* No previews */
					 NULL, /* No filter procedure */
					 NULL, /* I hope a NULL type list means we try to open any type. */
					 (NavCallBackUserData) x);
	if (err != noErr) {
		post("* NavGetFile returned error %ld", (long) err);
	} else {
		if (nrr.validRecord) {
			numItems = 99999;
			err = AECountItems(&(nrr.selection), &numItems);
			if (err != noErr) {
				post("* AECountItems returned error %ld", (long) err);
			} else {
				post("Ya selected %ld items", numItems);
				for (i = 1; i <= numItems; ++i) {
					err = AEGetNthPtr(&(nrr.selection), i, typeFSS, &theKeyword, &typeIGot,
									   &theSpec, sizeof(theSpec), &sizeIGot);
					if (err != noErr) {
						post("* AEGetNthDesc returned error %ld", (long) err);
					} else {
						if (sizeIGot != sizeof(theSpec)) {
							post("� surprise: size returned by AEGetNthDesc is %ld;", sizeIGot);
							post("  but sizeof(FSSpec) is only %ld", sizeof(theSpec));
						}
						
						f = FSp_fopen (&theSpec, "r"); 
						if (f == NULL) {
							post("� FSp_fopen returned NULL!");
						} else {
							SDIF_BeginRead(f);
							do_streamlist(f, "The one you chose", 0);
							fclose(f);
						}

						keywordAsString = ((char *) &theKeyword);
						post("Item %ld: keyword %c%c%c%c.", i, keywordAsString[0], 
							 keywordAsString[1], keywordAsString[2], keywordAsString[3]);
						
						post("FSSpec: volume %ld, parent ID %ld", (long) theSpec.vRefNum,
							 (long) theSpec.parID);
						
						PtoCstr(theSpec.name);
						post("   name: %s", theSpec.name);
					}
				}
			}
		} else {
			post("* validRecord is false");
		}

		post("* about to dispose reply");
		NavDisposeReply(&nrr);
	}
	
	
}

/********************  Buffer lookup ********************/

/* For now, just a linked list. */

#define Next(buffer) (((SDIFBufferPrivate *)buffer->internal)->next)


SDIFBuffer *MySDIFBufferLookupFunction(t_symbol *name) {
	SDIFBuffer *b;
	
	for (b = AllTheBuffers; b != NULL; b = Next(b)) {
		if (b->s_myname == name) return b;
	}
	return 0;
}

void AddNewBuffer(SDIFBuffer *b) {
	Next(b) = AllTheBuffers;
	AllTheBuffers = b;
}

void DeleteBuffer(SDIFBuffer *goner) {
	SDIFBuffer *b;

	if (goner == AllTheBuffers) {
		AllTheBuffers = Next(goner);
		return;
	}
	
	for (b = AllTheBuffers; Next(b) != NULL; b = Next(b)) {
		if (Next(b) == goner) {
			Next(b) = Next(goner);
			return;
		}
	}
}

void PrintAllTheBuffers(void) {
	SDIFBuffer *b;
	
	post("All the SDIF-buffers:");
	for (b = AllTheBuffers; b != NULL; b = Next(b)) {
		post("  %s", b->s_myname->s_name);
	}
}


/******************* Writing files *******************/

void SDIFbuffer_writefile(SDIFBuffer *x, Symbol *fileName) {
	SDIFresult r;
	FILE *fp;
	SDIFBufferPrivate *privateStuff;
	SDIFmem_Frame f;

	

	post("** SDIFbuffer_writefile (%p, %s)", x, fileName->s_name);

	privateStuff = (SDIFBufferPrivate *) x->internal;
	if (privateStuff->head == 0) {
		post("� SDIFbuffer %s is empty; not writing", x->s_myname->s_name);
		return;
	}
	
	r = SDIF_OpenWrite(fileName->s_name, &fp);
	if (r) {
		post("� SDIFbuffer: couldn't open file %s for writing: %s",
			 fileName->s_name, SDIF_GetErrorString(r));
		return;
	}
	
	for (f = privateStuff->head; f != 0; f = f->next) {
		r = SDIFmem_WriteFrame(fp, f);
		if (r) {
			post("� SDIFbuffer: problem writing frame to file %s: %s",
				 fileName->s_name, SDIF_GetErrorString(r));
			break;
		}
	}
		 
	r = SDIF_CloseWrite(fp);
	if (r) {
		post("� SDIFbuffer: problem closing file %s: %s",
			 fileName->s_name, SDIF_GetErrorString(r));
	}
}



#if 0

/***************** Adding data to the buffer in real time  ****************/


//****************************************************************
//****************************************************************
//****************************************************************
//****************************************************************

/* Tims stuff for one frame in RT 
   Cleaned up by Matt 021120.
*/

void RTInitFrameHeader(SDIFmem_Frame frame);


void SDIFbuffer_realtimeframe(
	SDIFBuffer *x, 
	Symbol *mess,
	int argc, 
	Atom *argv)
{
	int i;
	SDIF_FrameHeader *frame_header;
	SDIFmem_Frame previous, current, first;
	SDIFBufferPrivate *privateStuff;
	SDIF_MatrixHeader *matrix_header;
	int rows;
	int columns;
	SDIFmem_Matrix matrix_list;
	float *data, *dataptr;
	
	// Make a frame header.
	//frame_header = (SDIF_FrameHeader*)my_getbytes(sizeof(SDIF_FrameHeader));
	

	// Make a frame linked list item.
	current = SDIFmem_CreateEmptyFrame();
	if (current == 0) {
		post("� SDIF-buffer: realtimeframe: no memory for new frame.");
		return;
	}
	
	RTInitFrameHeader(current);

	
	// Link forward.
	privateStuff = (SDIFBufferPrivate *) x->internal;
	first = privateStuff->head;
	previous = privateStuff->tail;
	
	if (previous == NULL) // Then we have empty list.
	{
		first = current;
		previous = current;
		privateStuff->head = current;
		privateStuff->tail = current;
		
		//current->previous = NULL;
		current->next = NULL;
		 
	}
	else
	{
		// Link backward.
		//current->previous = previous;
		privateStuff->tail = current;
		
		// Link forward.
		previous->next = current;
	}
	
	
	// Determine size of matrix.
	// WE expect: time index 0 freq0 amp0 index1 freq1 amp1 etc.
	 rows = (argc -1) / 3;

	 columns = 4;
	



	// make matrix linked list.
	matrix_list = SDIFmem_CreateEmptyMatrix();
	matrix_list->header.matrixType[0] = '1';
	matrix_list->header.matrixType[1] = 'T';
	matrix_list->header.matrixType[2] = 'R';
	matrix_list->header.matrixType[3] = 'C';
	matrix_list->header.matrixDataType = SDIF_FLOAT32;
	matrix_list->header.rowCount = rows;
	matrix_list->header.columnCount = columns;

	matrix_list->next = NULL;
	// XXXXXXXXXXXXXXXXXX
	
	// Put matrix into the frame.
	current->matrices = matrix_list;

//typedef struct SDIFmemMatrixStruct {
//   SDIF_MatrixHeader header;
//    void *data;
//    struct SDIFmemMatrixStruct *next;
//} *SDIFmem_Matrix;

	// Get first argument, as time.
	current->header.time = argv[0].a_w.w_float;

	data = (float*)my_getbytes(sizeof(float)*rows*columns);
	dataptr = data;
	// 3 is for 3 arguments in index/freq/amp. start at 1 to miss time in arg 0
	for (i = 1; i < argc; i = i +3) 
	{
		*dataptr = argv[i].a_w.w_float;
		*dataptr++;

		*dataptr = argv[i+1].a_w.w_float;
		*dataptr++;
		
		*dataptr = argv[i+2].a_w.w_float;
		*dataptr++;
		
		*dataptr = 0.0;
		*dataptr++;
				
			
	}
	
	matrix_list->data = data;
	
	/* Now update all the buffer's state according to the newly read data */
	privateStuff = (SDIFBufferPrivate *) x->internal;
	privateStuff->head = first;
	privateStuff->tail = current;
	x->fileName = "RT";
	x->streamID = first->header.streamID;
	SDIF_Copy4Bytes(x->frameType, first->header.frameType);
	x->min_time = first->header.time;
	x->max_time = current->header.time;
	
	
	// Call a special function to remove glisses due to track death.
	//remove_glisses(current, previous);
	

}


/*****************************************
 *
 ******************************************/
// Take out glisses by just zeroing the partial. It is dirty but that the hell..
void remove_glisses(
	SDIFmem_Frame current,
	SDIFmem_Frame previous)
{
	//current->matrices->data
	int i;
	float *cur_data, *prev_data;
	int rows;
	
	post("current %f previous %f",current->header.time, previous->header.time);

	// See who has least number of rows.
	rows = current->matrices->header.rowCount;
	if (rows > previous->matrices->header.rowCount) 
		rows = previous->matrices->header.rowCount;
	
	post("rows %i ", rows);
	// Step through frames, if index changes and amp is large, then we zero the amplitude.
			cur_data = current->matrices->data;
		prev_data = previous->matrices->data;

	for (i = 0 ; i < rows; i ++)
	{
		// assume columns = 4, since I am short on time due to a demo. TJM
		// See if index changes.

		if (*cur_data != *prev_data)
		{ 
			post("gliss removed");
		 	// Then we have a gliss. Zero Both Amplitudes.
		 	*(cur_data+2) = 0.0;
		 	*(prev_data+2) = 0.0;
		 	// this way the gliss is silent (but still there.)
		 	
		 }
		 cur_data++;
		 cur_data++;
		 cur_data++;
		 cur_data++;
		 prev_data++;		 
		 prev_data++;		 
		 prev_data++;		 
		 prev_data++;		 
	}

	//current->matrices->header.columnCount = columns;
}


//*************************************************************************************
// Zero fills missing tracks.

void SDIFbuffer_realtimeframe2(
	SDIFBuffer *x, 
	Symbol *mess,
	int argc, 
	Atom *argv)
{
	int i;
	int int_index;
	SDIF_FrameHeader *frame_header;
	SDIFmem_Frame previous, current, first;
	SDIFBufferPrivate *privateStuff;
	SDIF_MatrixHeader *matrix_header;
	int rows;
	int columns;
	SDIFmem_Matrix matrix_list;
	float *data, *dataptr;
	
	int MAX_ROWS = 22;
	
	EnterCallback();

	// Make a frame header.
	//frame_header = (SDIF_FrameHeader*)my_getbytes(sizeof(SDIF_FrameHeader));
	

	// Make a frame linked list item.
	current = SDIFmem_CreateEmptyFrame();

	RTInitFrameHeader(current);

	
	// Link forward.
	privateStuff = (SDIFBufferPrivate *) x->internal;
	first = privateStuff->head;
	previous = privateStuff->tail;
	
	if (previous == NULL) // Then we have empty list.
	{
		first = current;
		previous = current;
		privateStuff->head = current;
		privateStuff->tail = current;
		
		//current->previous = NULL;
		current->next = NULL;
		 
	}
	else
	{
		// Link backward.
		//current->previous = previous;
		privateStuff->tail = current;
		
		// Link forward.
		previous->next = current;
	}
	
	
	// Determine size of matrix.
	// WE expect: time index0 freq0 amp0 index1 freq1 amp1 etc.
	// This means the indices are in monotone ascending order.
	// We will FILL in rows, for missing indices. If we get indices 1, 3, we have 4
	// rows: 0 1 2 3. Determine number of rows by LAST index.
	// argv[argc-1] is LAST element (last amp) so last index is at argc-3.
	 rows = (int)(argv[argc-3].a_w.w_float  + 0.00001) + 1;

	
	if (rows > MAX_ROWS) rows = MAX_ROWS;
	
	 columns = 4;
	



	// make matrix linked list.
	matrix_list = SDIFmem_CreateEmptyMatrix();
	matrix_list->header.matrixType[0] = '1';
	matrix_list->header.matrixType[1] = 'T';
	matrix_list->header.matrixType[2] = 'R';
	matrix_list->header.matrixType[3] = 'C';
	matrix_list->header.matrixDataType = SDIF_FLOAT32;
	matrix_list->header.rowCount = rows;
	matrix_list->header.columnCount = columns;

	matrix_list->next = NULL;
	// XXXXXXXXXXXXXXXXXX
	
	// Put matrix into the frame.
	current->matrices = matrix_list;

//typedef struct SDIFmemMatrixStruct {
//   SDIF_MatrixHeader header;
//    void *data;
//    struct SDIFmemMatrixStruct *next;
//} *SDIFmem_Matrix;

	// Get first argument, as time.
	current->header.time = argv[0].a_w.w_float;

	data = (float*)my_getbytes(sizeof(float)*rows*columns);
	dataptr = data;
	// First put in indices, and ZERO freq/amp/phase for all data to initialize it.
	for (i = 0; i < rows*columns; i = i + columns)
	{
		// ASSUME columns = 4. Kind of lame.
		// Index.
		*dataptr = 0.0;
		dataptr++;
		// Freq.
		*dataptr = 0.0;
		dataptr++;
		// Amp.
		*dataptr = 0.0;
		dataptr++;
		// Phase
		*dataptr = 0.0;
		dataptr++;	
	}
	// Now fill in the data we got from args.
	// Start at 1 to SKIP argv[0] which is frame time. Want index/freq/amp triplets.
	// Reason for inc i by 3 is to get each index/freq/amp triplet.
	for (i = 1; i < argc; i = i +3) 
	{
		// Make int. Add 0.00001 to get rid of 3.999999.... problem.
	     int_index = (int)(argv[i].a_w.w_float + 0.00001);
	     if (int_index < MAX_ROWS)// should be <= ??? 
		 {
		// Index
		data[columns*int_index] = argv[i].a_w.w_float;
		
		// Freq.
		data[columns*int_index + 1] = argv[i + 1].a_w.w_float;
		// Amp
		data[columns*int_index + 2] = argv[i + 2].a_w.w_float;
	    }
			
	}
	
	matrix_list->data = data;
	
	/* Now update all the buffer's state according to the newly read data */
	privateStuff = (SDIFBufferPrivate *) x->internal;
	privateStuff->head = first;
	privateStuff->tail = current;
	x->fileName = "RT";
	x->streamID = first->header.streamID;
	SDIF_Copy4Bytes(x->frameType, first->header.frameType);
	x->min_time = first->header.time;
	x->max_time = current->header.time;
	ExitCallback();	

}


// A but dirty. Makes a single matrix 1TRC frame at time 0, with streamID =0.
// For simple real time reading in of sdif data.

void RTInitFrameHeader(SDIFmem_Frame frame)
{
    
    frame->header.frameType[0] = '1';
    frame->header.frameType[1] = 'T';
    frame->header.frameType[2] = 'R';
    frame->header.frameType[3] = 'C';
    
          
    frame->header.time = 0.0;
    frame->header.streamID = 1;
    frame->header.matrixCount = 1;
  
}



void SDIFbuffer_realtimeHeader(
	SDIFBuffer *x, 
	Symbol *mess,
	int argc, 
	Atom *argv)
{
	
	int i;
	SDIF_FrameHeader *frame_header;
	SDIFmem_Frame previous, current, first;
	SDIFBufferPrivate *privateStuff;
	SDIF_MatrixHeader *matrix_header;
	int rows;
	int columns;
	int r;
	SDIFmem_Matrix matrix_list;
	float *data, *dataptr;
		
	//SDIFbuffer_doclear(x);
	
		privateStuff = (SDIFBufferPrivate *) x->internal;
	
	current = privateStuff->head;
		
	post("*******************************");
	while (current != NULL)
	{
	
	post("A Frame");
	
	post("Time: %f", current->header.time);
	
	post("Stream ID: %i", current->header.streamID);
	matrix_list = current->matrices;
	
	rows = matrix_list->header.rowCount;
	for (r = 0; r < rows; r++)
	{
		data = (float*)matrix_list->data;
		post("%f %f ",data[r*4 + 1], data[r*4 + 2]);
	
	}
	current = current->next;
	} 
	
	
}
#endif

