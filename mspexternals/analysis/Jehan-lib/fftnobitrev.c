/*
 * Copyright (c) 1992 Regents of the University of California.
 * All rights reserved.
 * The name of the
 * University may not be used to endorse or promote products derived
 * from this software without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 */

/*
 * ffft.c
 * real and complex forward and inverse fft's
 */
         
#include <math.h>
#include <stdlib.h>
#include <limits.h>
#include "fftnobitrev.h"

#define STORED // Use maketwiddle
// #define TFAST // Used in realfftnbr
// #define GOOD 

static float *twiddle = 0;
static int ncount; 
static struct {
		short ncache;
        float *twiddle;
        Boolean ffttype;
} c[CMAX];

/* maketwiddle */
static void maketwiddle(int n, Boolean notinverse, float* PtrMemory) {
	int i;
        
	/* cache twiddle factors */
	for (i=0; i<CMAX; ++i) {
		if (n==c[i].ncache && c[i].ffttype==notinverse) {
			twiddle = c[i].twiddle;
			return;
		}
	}
	{
		double pi = PI;
		float *tp;
	
		c[ncount].ncache = n;
		c[ncount].ffttype = notinverse;
		c[ncount].twiddle = &PtrMemory[ncount*n];
		twiddle = c[ncount].twiddle;

		if(!twiddle) 
			return;

		ncount = (ncount+1) % CMAX;
		tp = twiddle;

		/* compute twiddle factors */
		{
			double arg;
			double cos_arg, sin_arg;
                         
			if(notinverse)
				arg = 2.0*PI/n;
			else
				arg = -2.0*PI/n;
                                        
			*tp++ = 1.0f;
			*tp++ = 0.0f;
			cos_arg = cos(arg);
			sin_arg = sin(arg);
			*tp++ = cos_arg;
			*tp++ = sin_arg;
			tp -= 2;
			for (i=2; i<n-1; i++) {
				*(tp+2) = *tp *cos_arg - *(tp+1)*sin_arg;
				*(tp+3) = *tp *sin_arg + *(tp+1)*cos_arg;
				tp += 2;
			}
		}
	}
}

/*
 * complex fft
 * real and complex components are stored alternately in *a
 * n is number of points
 * calculation is in place 
 */
  
void fftComplexnbr(int n, float *a, Boolean notinverse, float* PtrMemory) {
	long i, j, mmax, m, N;
	float t, tr,tc;
    long istep;
        
    --a;
        
    N = n*2;
    {
		j=1;
		for (i=1; i<N; i+=2) {
			if(j>i) {
				t = a[j];
				a[j] = a[i];
				a[i] = t;
				t = a[i+1];
				a[i+1] = a[j+1];
				a[j+1] = t;
			}
			m = n;
			while (m>=2 && j>m) {
				j -= m;
				m >>= 1;
			}
			j += m;
		}
	}
#ifdef STORED
	maketwiddle(n,notinverse,PtrMemory);
#endif

	/* fft itself */
	{
		long uNQmq, NQmq;

#ifdef STORED
		float ur, uc;
#else
		double ur, uc, wpr, wpi, wpin, theta, wtemp;
#endif
		float ai,aiplus1;
		mmax = 2;
		NQmq = n;

#ifndef STORED
		theta = PI * 0.5;
		if(!notinverse)
			theta = - theta;
		wpin = 0.0;
#endif
                        
		while (N>mmax) {

#ifndef STORED
			wpi = wpin;
			wpin = sin(theta);
			wpr = 1.0 - wpin*wpin - wpin*wpin;
			theta *= 0.5;
			ur = 1.0;
			uc = 0.0;
#endif                
			istep = mmax<<1;

#ifdef GOOD
			for (j=1, uNQmq=0; j<mmax; j+=2, uNQmq+=NQmq) {
#ifdef STORED
 				ur = twiddle[uNQmq];
				uc = twiddle[uNQmq+1];
#endif
				for (i=j; i<=N; i+=istep) {
						int k = i+mmax;

						/* complex multiply of a by u */
						tr = (float)ur*a[k] - (float)uc*a[k+1];
						tc = (float)uc*a[k] + (float)ur*a[k+1];
						
						/* butterfly */
						a[k] = (ai=a[i]) - tr;
						a[i] = ai + tr;
						a[k+1] = (aiplus1=a[i+1]) - tc;
						a[i+1] = aiplus1+tc;
				}
#ifndef STORED
				ur = (wtemp = ur)*wpr - uc*wpi;
				uc = wtemp*wpi + uc*wpr;
#endif
			}                        
#else // no GOOD

			for (j=1, uNQmq=0; j<mmax; j+=2, uNQmq+=NQmq) {
				float d1r, d1c, *kkp, *kp, *ap;
#ifdef STORED
				ur = twiddle[uNQmq];
				uc = twiddle[uNQmq+1];
#endif
				tr = (float)ur*(d1r=a[j+mmax]);
				tc = (float)uc*(d1c=a[j+mmax+1]);

				for (i=j, kp=&a[mmax+j], ap=&a[j], kkp=&a[mmax+j+istep]; 
						i<=(N-istep); i+=istep, ap+=istep, kp+=istep, kkp+=istep) {

					/* complex multiply of a by u */
					tr -= tc;
					tc = (float)uc*d1r + (float)ur*d1c;

					/* butterfly */
					d1r = *kkp;
					d1c = kkp[1];
					*kp = (ai=*ap) - tr;
					*ap = ai + tr;
					kp[1] = (aiplus1=ap[1]) - tc;
					ap[1] = aiplus1+tc;
					tr = (float)ur*d1r; 
					tc = (float)uc*d1c;
				}
				{  
					int k = i+mmax;
					
					/* complex multiply of a by u */
					tr -= tc;
					tc = (float)uc*d1r + (float)ur*d1c;
					
					/* butterfly */
					a[k] = (ai=a[i]) - tr;
					a[i] = ai + tr;
					a[k+1] = (aiplus1=a[i+1]) - tc;
					a[i+1] = aiplus1+tc;
				}

#ifndef STORED
				ur = (wtemp = ur)*wpr - uc*wpi;
				uc = wtemp*wpi + uc*wpr;
#endif
			}
#endif

			mmax = istep;
	 		NQmq >>= 1;                     
		}
	}
	if (!notinverse) {
		float scale = 1.0f/n;
	
		for (i=1; i<=n*2; ++i)
			a[i] *= scale;
	}
}

/* 
 * realfftnbr
 * for real valued fft's this scrambles and unscrambles to use
 * a complexfft (above) of half the size
 */ 
 
void realfftnbr(int n, float *a, Boolean notinverse, float* PtrMemory) {
	int i, i1, i2, i3, i4, n2p3;
	float c1=0.5f, c2, h1r, h1i, h2r, h2i;

#ifdef TFAST
	float wr, wi;
#else
	double wr, wi, wpr, wpi, wtemp, theta;
	theta = PI/n;
#endif  

	if (notinverse) {
		c2= -0.5f;
		fftComplexnbr(n,a,notinverse,PtrMemory);
	} else {
		c2=0.5f;
#ifndef TFAST
		theta = -theta;
#endif
	}
	--a;
	maketwiddle(2*n,notinverse,PtrMemory);

#ifndef TFAST
	wtemp = sin(0.5*theta);
	wpr = -2.0 *wtemp*wtemp;
	wpi = sin(theta);
	wr = 1.0+wpr;
	wi = wpi;
#endif  

	n2p3 = 2*n+3;
	for (i=2; i<=n/2; ++i) {
		i4 = 1+ (i3=n2p3-(i2=1+(i1=i+i-1)));
		h1r = c1*(a[i1] + a[i3]);
		h1i = c1*(a[i2] - a[i4]);
		h2r = -c2*(a[i2] + a[i4]);
		h2i = c2*(a[i1]-a[i3]);

#ifdef TFAST
		wr = twiddle[2*(i-1)];
		wi = twiddle[2*(i-1)+1];
#endif
		a[i1] = h1r+wr*h2r-wi*h2i;
		a[i2] = h1i+wr*h2i+wi*h2r;
		a[i3] = h1r-wr*h2r+wi*h2i;
		a[i4] = -h1i+wr*h2i+wi*h2r;

#ifndef TFAST
		wr = (wtemp=wr)*wpr -wi*wpi+wr;
		wi = wi*wpr+wtemp*wpi+wi;
#endif
	}
		
	if (notinverse) {
		a[1] = (h1r=a[1])+a[2];
		a[2] = h1r-a[2];
	} else {
		a[1] = c1*((h1r=a[1])+a[2]);
		a[2] = c1*(h1r-a[2]);
		fftComplexnbr(n, a+1, notinverse, PtrMemory);
	}        
}

/* fftRealfastnbr */
void fftRealfastnbr(int n, float *r, float* PtrMemory) {
	realfftnbr(n/2, r, TRUE, PtrMemory);
}  

/* ifftRealfastnbr */
void ifftRealfastnbr(int n, float *rc, float* PtrMemory) {
	realfftnbr(n/2, rc, FALSE, PtrMemory);
}
