/* 
	jit.byteorder.h

	Copyright 2001-2002 - Cycling '74
	Joshua Kit Clayton jkc@cycling74.com
	
*/

#ifndef __JIT_BYTEORDER_H__
#define __JIT_BYTEORDER_H__

#ifdef WIN32
#define LITTLE_ENDIAN
#else
#define BIG_ENDIAN 
#endif

#define SWAP16(x) ((short)(((((unsigned short)(x))>>8)&0x00ff)+((((unsigned short)(x))<<8)&0xff00)))
#define SWAP32(x) ((long)(((((unsigned long)(x))>>24L)&0x000000ff)+((((unsigned long)(x))>>8L)&0x0000ff00)+ \
					((((unsigned long)(x))<<24L)&0xff000000)+((((unsigned long)(x))<<8L)&0x00ff0000)))
#define SWAPF32 swapf32
#define SWAPF64 swapf64

#ifdef BIG_ENDIAN
#define LE_I16(x) 		SWAP16(x)
#define LE_I32(x) 		SWAP32(x)
#define LE_F32(x) 		SWAPF32(x)
#define LE_F64(x) 		SWAPF64(x)
#define BE_I16(x) 		(x)
#define BE_I32(x) 		(x)
#define BE_F32(x)		(x)
#define BE_F64(x) 		(x)
#define FOUR_CHAR(x)	(x)
#else
#define LE_I16(x) 		(x)
#define LE_I32(x) 		(x)
#define LE_F32(x) 		(x)
#define LE_F64(x) 		(x)
#define BE_I16(x) 		SWAP16(x)
#define BE_I32(x) 		SWAP32(x)
#define BE_F32(x) 		SWAPF32(x)
#define BE_F64(x) 		SWAPF64(x)
#define FOUR_CHAR(x)	SWAP32(x)
#endif

float swapf32(float f);
double swapf64(double f);

#endif // __JIT_BYTEORDER_H__