/*
Copyright (c) 1999,2000-07.  The Regents of the University of California (Regents).
All Rights Reserved.

Permission to use, copy, modify, and distribute this software and its
documentation for educational, research, and not-for-profit purposes, without
fee and without a signed licensing agreement, is hereby granted, provided that
the above copyright notice, this paragraph and the following two paragraphs
appear in all copies, modifications, and distributions.  Contact The Office of
Technology Licensing, UC Berkeley, 2150 Shattuck Avenue, Suite 510, Berkeley,
CA 94720-1620, (510) 643-7201, for commercial licensing opportunities.

Written by Tristan Jehan, The Center for New Music and Audio Technologies,
University of California, Berkeley.

     IN NO EVENT SHALL REGENTS BE LIABLE TO ANY PARTY FOR DIRECT, INDIRECT,
     SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING LOST PROFITS,
     ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS DOCUMENTATION, EVEN IF
     REGENTS HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

     REGENTS SPECIFICALLY DISCLAIMS ANY WARRANTIES, INCLUDING, BUT NOT
     LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
     FOR A PARTICULAR PURPOSE. THE SOFTWARE AND ACCOMPANYING
     DOCUMENTATION, IF ANY, PROVIDED HEREUNDER IS PROVIDED "AS IS".
     REGENTS HAS NO OBLIGATION TO PROVIDE MAINTENANCE, SUPPORT, UPDATES,
     ENHANCEMENTS, OR MODIFICATIONS.
     
    
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
NAME: peqbank~
DESCRIPTION: Bank of biquad filters in series with analog-like control parameters based on shelving or parametric EQ (or low-level control in the biquad coefficient domain)
AUTHORS: Tristan Jehan, Matt Wright, Andy Schmeder
COPYRIGHT_YEARS: 1999,2000,01,02,03,04,05,06,07,09
DRUPAL_NODE: /patch/4026
SVN_REVISION: $LastChangedRevision$
PUBLICATION: ICMC99 paper | http://www.cnmat.berkeley.edu/ICMC99/papers/MSP-filters/filticmc.html
VERSION 1.0: Tristan's initial version 
VERSION 1.1: Minor polishing by Matt Wright, 12/10/99 (version, tellmeeverything)
VERSION 1.2: Major fix of smooth mode disaster, Matt Wright 5/4/2000
VERSION 1.3: Fixed fast mode, 7/11/2000
VERSION 1.5 Expires June 2002, 
VERSION 1.6 never expires
VERSION 1.7 expires March 1, 2003
VERSION 1.8 fixes peqbank_free bug
VERSION 1.9 Added "biquads" message; expires 12/1/3
VERSION 2.0 Never expires
VERSION 2.0.1: Force Package Info Generation
VERSION 2.1: Fixed bug of overwriting input signal vector with the filtered output
VERSION 2.2: Added "bank" message as a synonym for "list", dsp_free fixed -mz
VERSION 2.3: Tried to make peqbank_compute() be reentrant.
VERSION 2.3.1 Fixed crash caused by maxelem() and peqbank_reset() calling peqbank_free() --JM
VERSION 2.4: Support for multiple channels, vector optimization, built with Intel CC
VERSION 2.5: Support for internal generation of biquad cascade for high/low pass cheby/butterworth filter
VERSION 2.5.1: Fix denormal problem
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@  


TO-DO:  Include b_nbpeq and b_start in the atomic pointer-swapping scheme
        
        Double-precision coefficients, state variables, intra-cascade signal passing
            (in alternate, slower perform routine)

*/
#define NAME "peqbank~"
#define DESCRIPTION "Bank of biquad filters in series with analog-like control parameters based on shelving or parametric EQ (or low-level control in the biquad coefficient domain)"
#define AUTHORS "Tristan Jehan, Matt Wright, Andy Schmeder"
#define COPYRIGHT_YEARS "1999,2000-07,9,12,13"




/* How smooth mode works:

	Conceptually, it's simple.  Whenever coefficients change, we linearly interpolate the 
	coefficients from the old values to the new values over one signal vector.
	
	How do we know when they've changed and keep track of the old and new values?
	It's hairy because the audio interrupt might
	come in the middle of computing the new coefficients.  In the worst case, we get new
	parameters and compute new coefficients, then get new new parameters, and, in the
	middle of computing the new new coefficients, get an audio interrupt.  From this case
	we see that we need three buffers:  the old coefficients (that we used on the last
	vector), the new coefficients (that we just computed), and temporary space to compute
	a new set of coefficients.
	
	We have four pointers to these three arrays:
		coeff - the values we'll use next time
		oldcoeff - the values we used last time
		newcoeff - where we compute new values
		freecoeff - If coeff==oldcoeff, freecoeff points to the third buffer.
		
	The default state, when the coefficients haven't changed recently, is that
	coeff==oldcoeff, and the remaining two pointers point to the remaining two buffers.
	
	After new coefficients are computed we make them take effect by setting
	coeff = newcoeff; newcoeff = freecoeff; freecoeff = 0;
	
	But maybe we already computed new coefficients since the last time the perform routine
	happened.  In that case coeff will be the "old new" coefficients, computed recently
	but not yet actually used by a perform routine.  In that case we simply swap coeff and newcoeff.
	
	The perform routine sees that coeff != oldcoeff and interpolates.  Then it sets
	freecoeff = oldcoeff; oldcoeff = coeff;

*/


#include "ext.h"
#include "ext_obex.h"

#include "version.h"

#include "z_dsp.h"
#include <Memory.h>
#include <math.h>
#include <stdio.h>

#ifdef WIN_VERSION
#define sinhf sinh
#define sqrtf sqrt
#define tanf tan
#define expf exp
#endif

// switched to simple denormal check for doubles to avoid cruchy sound
#define FLUSH_TO_ZERO(fv) (fabs(fv)<1e-20f)?0.0:(fv)

//#define FLUSH_TO_ZERO(fv) (((*(unsigned int*)&(fv))&0x7f800000)==0)?0.0f:(fv)


#undef PI  /* so we can use the one below instead of the one from math.h */
#undef TWOPI /* ditto */

#define RES_ID	7007
#define LOG_10	2.30258509299405f	// ln(10)
#define LOG_2	0.69314718055994f	// ln(2)
#define LOG_22	0.34657359027997f	// ln(2)/2
#define PI		3.14159265358979f	// PI
#define PI2		9.86960440108936f	// PI squared
#define TWOPI	6.28318530717959f	// 2 * PI
#define SMALL   0.000001
#define NBCOEFF 5
#define NBPARAM 5
#define FAST	1
#define SMOOTH  0
#define MAXELEM 10

t_class *peqbank_class;

typedef struct _peqbank {

	t_pxobject b_obj;

	double *param;		// Ptr on stored parameters
	double *oldparam;	// Ptr on stored old parameters
	
	double *coeff;		// See large comment above for explanation of these four pointers
	double *oldcoeff;
	double *newcoeff;
	double *freecoeff;

#ifdef DEBUG	
	double testcoeff3;  // Matt sanity check
#endif	
	double b_Fs;			// Sample rate

    int b_channels;     // Number of channels to process in parallel
	int b_max;			// Number max of peq filters (used to allocate memory)
	int b_nbpeq;		// Current number of peq filters
	int b_start;		// 0=shelf, 5=no shelf
	int b_version;		// SMOOTH (0) or FAST (1) 

	double *b_ym1;		// Ptr on y minus 1 per biquad, per channel
	double *b_ym2;		// Ptr on y minus 2 per biquad, per channel
	double *b_xm1;		// Ptr on x minus 1 per biquad, per channel
	double *b_xm2;		// Pyr on x minus 2 per biquad, per channel
    
    //t_float** s_vec_in;    // input vectors in multi-channel mode
    //t_float** s_vec_out;   // output vectors
    //int s_n;
	
	t_atom *myList;		// Copy of coefficients as Atoms
	void *b_outlet;		// List of biquad coefficients
	
	int already_peqbank_compute;		// Flag for whether we're currently computing new coefficients
	int need_to_recompute;		// Flag for whether we need to recompute new coefficients

} t_peqbank;

t_symbol *ps_maxelem, *ps_shelf, *ps_peq, *ps_fast, *ps_smooth, *ps_channels, *ps_highpass, *ps_lowpass;

//t_int *peqbank_perform_smooth(t_int *w);
//t_int *peqbank_perform_fast(t_int *w);
//t_int *peqbank_perform_smooth_multi(t_int *w);
//t_int *peqbank_perform_fast_multi(t_int *w);
//t_int *do_peqbank_perform_fast(t_int *w, float *mycoeffs);
//void do_peqbank_perform_fast_multi(t_peqbank *w, float *mycoeffs);
void peqbank_perform64_fast_multi(t_peqbank *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam);
void do_peqbank_perform64_fast_multi(t_peqbank *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, double *mycoeff);
void peqbank_perform64_smooth_multi(t_peqbank *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam);
void peqbank_perform64_fast(t_peqbank *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam);
void do_peqbank_perform64_fast(t_peqbank *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, double *mycoeff);

void peqbank_clear(t_peqbank *x);
//void peqbank_dsp(t_peqbank *x, t_signal **sp, short *connect);
void peqbank_dsp64(t_peqbank *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags);
void peqbank_reset(t_peqbank *x);
int maxelem(t_peqbank *x, t_symbol *s, short argc, t_atom *argv, int rest);
int shelf(t_peqbank *x, t_symbol *s, short argc, t_atom *argv, int rest);
int peq(t_peqbank *x, t_symbol *s, short argc, t_atom *argv, int rest);
void peqbank_maxelem(t_peqbank *x, t_symbol *s, short argc, t_atom *argv);
void peqbank_shelf(t_peqbank *x, t_symbol *s, short argc, t_atom *argv);
void peqbank_peq(t_peqbank *x, t_symbol *s, short argc, t_atom *argv);
void peqbank_fast(t_peqbank *x, t_symbol *s, short argc, t_atom *argv);
void peqbank_smooth(t_peqbank *x, t_symbol *s, short argc, t_atom *argv);
void peqbank_list(t_peqbank *x, t_symbol *s, short argc, t_atom *argv);
void peqbank_biquads(t_peqbank *x, t_symbol *s, short argc, t_atom *argv);
void peqbank_cheby(t_peqbank *x, t_symbol *s, short argc, t_atom *argv);
int peqbank_cheby_coeffs(t_peqbank *x, double freq, int type, int order, double ripple);
void peqbank_init(t_peqbank *x);
void peqbank_assist(t_peqbank *x, void *b, long m, long a, char *s);
void *peqbank_new(t_symbol *s, short argc, t_atom *argv);
void peqbank_allocmem(t_peqbank *x);
void peqbank_freemem(t_peqbank *x);
void peqbank_free(t_peqbank *x);
void peqbank_compute(t_peqbank *x);
void swap_in_new_coeffs(t_peqbank *x);
void compute_parameq(t_peqbank *x, int index); 
void compute_shelf(t_peqbank *x); 
double pow10(double x);
double pow2(double x);
void peqbank_tellmeeverything(t_peqbank *x);

int test_normal_state(t_peqbank *x);
int test_newcoeffs_state(t_peqbank *x);

int main(void){
        version_post_copyright();

	ps_maxelem = gensym("maxelem");
	ps_shelf   = gensym("shelf");
	ps_peq     = gensym("peq");
	ps_fast    = gensym("fast");
	ps_smooth  = gensym("smooth");
    ps_channels = gensym("channels");
	ps_highpass = gensym("highpass");
	ps_lowpass = gensym("lowpass");
	
	peqbank_class = class_new("peqbank~", (method)peqbank_new, (method)peqbank_free,
			(short)sizeof(t_peqbank), 0L, A_GIMME, 0);

	{
#ifdef EXPIRE
#define EXPIRATION_STRING "Expires December 1, 2003"
		DateTimeRec date;
		GetTime(&date);
		object_post((t_object *)x, EXPIRATION_STRING);
		if(!((date.year==2002) || (date.year==2003 && date.month < 12)))
		{
			object_post((t_object *)x, "Expired");
		}
		else
#else
		post ("Never expires.");
#endif
		//class_addmethod(peqbank_class, (method)peqbank_dsp, "dsp", A_CANT, 0);
        class_addmethod(peqbank_class, (method)peqbank_dsp64, "dsp64", A_CANT, 0);
	}
	class_addmethod(peqbank_class, (method)peqbank_reset, "reset", A_GIMME, 0);
	class_addmethod(peqbank_class, (method)peqbank_list, "list", A_GIMME, 0);
	class_addmethod(peqbank_class, (method)peqbank_list, "bank", A_GIMME, 0); //This is a better name than "list," because it isn't a reservered word in Max. -mz
	class_addmethod(peqbank_class, (method)peqbank_maxelem, "maxelem", A_GIMME, 0);
	class_addmethod(peqbank_class, (method)peqbank_shelf, "shelf", A_GIMME, 0);
	class_addmethod(peqbank_class, (method)peqbank_peq, "peq", A_GIMME, 0);
	class_addmethod(peqbank_class, (method)peqbank_fast, "fast", A_GIMME, 0);
	class_addmethod(peqbank_class, (method)peqbank_smooth, "smooth", A_GIMME, 0);
	class_addmethod(peqbank_class, (method)peqbank_clear, "clear", 0);
	class_addmethod(peqbank_class, (method)peqbank_assist, "assist", A_CANT, 0);
	class_addmethod(peqbank_class, (method)version, "version", 0);
	class_addmethod(peqbank_class, (method)peqbank_tellmeeverything, "tellmeeverything", 0);
	class_addmethod(peqbank_class, (method)peqbank_biquads, "biquads", A_GIMME);
	class_addmethod(peqbank_class, (method)peqbank_cheby, "highpass", A_GIMME);
	class_addmethod(peqbank_class, (method)peqbank_cheby, "lowpass", A_GIMME);
	
	class_dspinit(peqbank_class);

	class_register(CLASS_BOX, peqbank_class);
	
	return 0;

	//rescopy('STR#', RES_ID);
}

void peqbank_tellmeeverything(t_peqbank *x) {
	int i;
	
	version(x);

    if (x->b_version == SMOOTH) {
    	object_post((t_object *)x, "  Smooth mode: coefficients linearly interpolated over one MSP vector");
    } else if (x->b_version == FAST) {
    	object_post((t_object *)x, "  Fast mode: no interpolation when filter parameters change");
    } else {
    	object_post((t_object *)x, "  ERROR: object is in neither FAST mode nor SMOOTH mode!");
    }
    
    object_post((t_object *)x, "  Channels: %d", x->b_channels);
    
    post("  coeff = %p, oldcoeff = %p, newcoeff = %p, freecoeff = %p",
    	x->coeff, x->oldcoeff, x->newcoeff, x->freecoeff);
    
    object_post((t_object *)x, "  Allocated enough memory for %ld filters", x->b_max);
    if (x->b_start == 0) {
    	post("  Shelving EQ: %.2f dB, %.2f dB, %.2f dB, %.2f Hz, %.2f Hz",
    		 x->param[0], x->param[1], x->param[2], x->param[3], x->param[4]);
    	post("  (%.1f dB below %.0f Hz, %.1f dB between, %.1f dB above %.0f Hz)",
    		 x->param[0], x->param[3], x->param[1], x->param[2], x->param[4]);
    	object_post((t_object *)x, "  (biquad: %f %f %f %f %f)", x->coeff[0], x->coeff[1], x->coeff[2], x->coeff[3], x->coeff[4]);
    } else {
    	object_post((t_object *)x, "  No shelving EQ.");
    }
    object_post((t_object *)x, "  Computing %ld PEQ filters:", x->b_nbpeq);
    for (i = 1; i <= x->b_nbpeq; ++i) {
    	post("   %ld: Ctr %.2fHz, BW %.2f oct, %.1fdB at DC, %.1fdB at ctr, %.1fdB at BW edges (biquad: %f %f %f %f %f)", 
    		 i, x->param[i*5], x->param[i*5+1], x->param[i*5+2], x->param[i*5+3], x->param[i*5+4],
    		 x->coeff[i*5], x->coeff[i*5+1], x->coeff[i*5+2], x->coeff[i*5+3], x->coeff[i*5+4]);
    }
    
   
    if (x->coeff == x->oldcoeff) {
    	if (!test_normal_state(x)) post("Assertions failed in tme, x->coeff == x->oldcoeff");
    } else {
    	if (!test_newcoeffs_state(x)) post("Assertions failed in tme, x->coeff != x->oldcoeff");
    }
}


int posted = 0;

void peqbank_perform64_smooth_multi(t_peqbank *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam)
{
    peqbank_perform64_fast_multi(x, dsp64, ins, numins, outs, numouts, sampleframes, flags, userparam);
}
/*
t_int *peqbank_perform_smooth_multi(t_int *w) {

   
    // smooth multi channel isn't supported yet, just use fast so at least it doesn't break... -aws
    return peqbank_perform_fast_multi(w);
    
}
*/
void peqbank_perform64_smooth(t_peqbank *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam)
{
    double *in  = ins[0];
    double *out = outs[0];
    //t_peqbank *x = (t_peqbank *)(w[3]);
    //int n        = (int)(w[4]);
    int n = sampleframes;
    double *mycoeff;
    
    // In overdrive, it's possible that this perform routine could be interrupted (e.g.,
    // by a MIDI event) by new coefficients being calculated.  So we snapshot a local
    // copy of x->coeff
    // Nowhere else is x->oldcoeff set, so we don't have to worry about that.
    
    mycoeff = x->coeff;
    
#ifdef DEBUG
    if (x->oldcoeff[3] != x->testcoeff3) {
        if (posted == 1) {
            object_post((t_object *)x, "testcoeff3 is %f, but x->oldcoeff[3] is %f!", x->testcoeff3, x->oldcoeff[3]);
        }
        ++ posted;
        posted = posted %100;
    }
#endif
    
    if (mycoeff == x->oldcoeff) {
        // Coefficients haven't changed, so no need to interpolate
        
#ifdef DEBUG
        if (!test_normal_state(x)) post("peqbank_perform: mycoeff==x->oldcoeff");
#endif
        
        //return do_peqbank_perform_fast(w, mycoeff);
        do_peqbank_perform64_fast(x, dsp64, ins, numins, outs, numouts, sampleframes, flags, mycoeff);
    } else {
        
        // Biquad with linear interpolation: smooth-biquad~
        int i, j, k=0;
        double rate = 1.0f/n;
        double i0, i1, i2, i3;
        double a0, a1, a2, b1, b2;
        double a0inc, a1inc, a2inc, b1inc, b2inc;
        double y0, y1;
        
        
#ifdef DEBUG
        if (!test_newcoeffs_state(x)) post("peqbank_perform: mycoeff!=x->oldcoeff");
#endif
        
        
        // First copy input vector to output vector (below we'll filter the output in-place
        if (out != in) {
            for (i=0; i<n; ++i) out[i] = in[i];
        }
        
        // Cascade of Biquads
        for (j=x->b_start; j<(x->b_nbpeq+1)*NBCOEFF; j+=NBCOEFF) {
            
            i2 = x->b_xm2[k];
            i3 = x->b_xm1[k];
            y0 = x->b_ym2[k];
            y1 = x->b_ym1[k];
            
            // Interpolated values
            a0 = x->oldcoeff[j  ];
            a1 = x->oldcoeff[j+1];
            a2 = x->oldcoeff[j+2];
            b1 = x->oldcoeff[j+3];
            b2 = x->oldcoeff[j+4];
            
            // Incrementation values
            a0inc = (mycoeff[j  ] - a0) * rate;
            a1inc = (mycoeff[j+1] - a1) * rate;
            a2inc = (mycoeff[j+2] - a2) * rate;
            b1inc = (mycoeff[j+3] - b1) * rate;
            b2inc = (mycoeff[j+4] - b2) * rate;
            
            for (i=0; i<n; i+=4) {
                
                out[i  ] = y0 = (a0 * (i0 = out[i  ])) + (a1 * i3) + (a2 * i2) - (b1 * y1) - (b2 * y0);
                a1 += a1inc; a2 += a2inc; a0 += a0inc; b1 += b1inc; b2 += b2inc;
                
                out[i+1] = y1 = (a0 * (i1 = out[i+1])) + (a1 * i0) + (a2 * i3) - (b1 * y0) - (b2 * y1);
                a1 += a1inc; a2 += a2inc; a0 += a0inc; b1 += b1inc; b2 += b2inc;
                
                out[i+2] = y0 = (a0 * (i2 = out[i+2])) + (a1 * i1) + (a2 * i0) - (b1 * y1) - (b2 * y0);
                a1 += a1inc; a2 += a2inc; a0 += a0inc; b1 += b1inc; b2 += b2inc;
                
                out[i+3] = y1 = (a0 * (i3 = out[i+3])) + (a1 * i2) + (a2 * i1) - (b1 * y0) - (b2 * y1);
                a1 += a1inc; a2 += a2inc; a0 += a0inc; b1 += b1inc; b2 += b2inc;
                
            } // Interpolation loop
            
            x->b_xm2[k] = FLUSH_TO_ZERO(i2);
            x->b_xm1[k] = FLUSH_TO_ZERO(i3);
            x->b_ym2[k] = FLUSH_TO_ZERO(y0);
            x->b_ym1[k] = FLUSH_TO_ZERO(y1);
            k ++;
            
        } // cascade loop
        
        
        // Now that we've made it to the end of the signal vector, the "old" coefficients are
        // the ones we just used.  The previous "old" coeffients, which we've now finished
        // interpolating away from, are not needed any more.
        
        if (x->freecoeff != 0) {
            object_post((t_object *)x, "peqbank~: disaster (smooth)!  freecoeff should be zero now!");
        }
        
#ifdef DEBUG	
        x->testcoeff3 = x->oldcoeff[3];
#endif
        
        x->freecoeff = x->oldcoeff;
        x->oldcoeff = mycoeff;		
    }
}
/*
t_int *peqbank_perform_smooth(t_int *w) {
    
	t_float *in  = (t_float *)(w[1]);
	t_float *out = (t_float *)(w[2]);
	t_peqbank *x = (t_peqbank *)(w[3]);
	int n        = (int)(w[4]);
	float *mycoeff;
	
	// In overdrive, it's possible that this perform routine could be interrupted (e.g., 
	// by a MIDI event) by new coefficients being calculated.  So we snapshot a local
	// copy of x->coeff
	// Nowhere else is x->oldcoeff set, so we don't have to worry about that.
	
	mycoeff = x->coeff;

#ifdef DEBUG	
	if (x->oldcoeff[3] != x->testcoeff3) {
		if (posted == 1) {
			object_post((t_object *)x, "testcoeff3 is %f, but x->oldcoeff[3] is %f!", x->testcoeff3, x->oldcoeff[3]);
		}
		++ posted;
		posted = posted %100;
	}
#endif

	if (mycoeff == x->oldcoeff) {
		// Coefficients haven't changed, so no need to interpolate
		
#ifdef DEBUG		
		if (!test_normal_state(x)) post("peqbank_perform: mycoeff==x->oldcoeff");
#endif
		
		return do_peqbank_perform_fast(w, mycoeff);
	} else {

		// Biquad with linear interpolation: smooth-biquad~
		int i, j, k=0;
		float rate = 1.0f/n;
		float i0, i1, i2, i3;
		float a0, a1, a2, b1, b2;
		float a0inc, a1inc, a2inc, b1inc, b2inc;
		float y0, y1;
		

#ifdef DEBUG		
		if (!test_newcoeffs_state(x)) post("peqbank_perform: mycoeff!=x->oldcoeff");
#endif


		// First copy input vector to output vector (below we'll filter the output in-place
		if (out != in) {
			for (i=0; i<n; ++i) out[i] = in[i];
		}

		// Cascade of Biquads
		for (j=x->b_start; j<(x->b_nbpeq+1)*NBCOEFF; j+=NBCOEFF) {
			
			i2 = x->b_xm2[k]; 
			i3 = x->b_xm1[k];
			y0 = x->b_ym2[k]; 
			y1 = x->b_ym1[k];	
			
			// Interpolated values
			a0 = x->oldcoeff[j  ];
			a1 = x->oldcoeff[j+1];
			a2 = x->oldcoeff[j+2];
			b1 = x->oldcoeff[j+3];	
			b2 = x->oldcoeff[j+4];
					
			// Incrementation values
			a0inc = (mycoeff[j  ] - a0) * rate;
			a1inc = (mycoeff[j+1] - a1) * rate;
			a2inc = (mycoeff[j+2] - a2) * rate;
			b1inc = (mycoeff[j+3] - b1) * rate;
			b2inc = (mycoeff[j+4] - b2) * rate;
			
			for (i=0; i<n; i+=4) {

				out[i  ] = y0 = (a0 * (i0 = out[i  ])) + (a1 * i3) + (a2 * i2) - (b1 * y1) - (b2 * y0);
				a1 += a1inc; a2 += a2inc; a0 += a0inc; b1 += b1inc; b2 += b2inc;
		
				out[i+1] = y1 = (a0 * (i1 = out[i+1])) + (a1 * i0) + (a2 * i3) - (b1 * y0) - (b2 * y1);
				a1 += a1inc; a2 += a2inc; a0 += a0inc; b1 += b1inc; b2 += b2inc;

				out[i+2] = y0 = (a0 * (i2 = out[i+2])) + (a1 * i1) + (a2 * i0) - (b1 * y1) - (b2 * y0);
				a1 += a1inc; a2 += a2inc; a0 += a0inc; b1 += b1inc; b2 += b2inc;

				out[i+3] = y1 = (a0 * (i3 = out[i+3])) + (a1 * i2) + (a2 * i1) - (b1 * y0) - (b2 * y1);
				a1 += a1inc; a2 += a2inc; a0 += a0inc; b1 += b1inc; b2 += b2inc;
		
			} // Interpolation loop						

			x->b_xm2[k] = FLUSH_TO_ZERO(i2);
			x->b_xm1[k] = FLUSH_TO_ZERO(i3);
			x->b_ym2[k] = FLUSH_TO_ZERO(y0);
			x->b_ym1[k] = FLUSH_TO_ZERO(y1);
			k ++;
			
		} // cascade loop
		
		
		// Now that we've made it to the end of the signal vector, the "old" coefficients are
		// the ones we just used.  The previous "old" coeffients, which we've now finished
		// interpolating away from, are not needed any more.
		
		if (x->freecoeff != 0) {
			object_post((t_object *)x, "peqbank~: disaster (smooth)!  freecoeff should be zero now!");
		}
		
#ifdef DEBUG	
		x->testcoeff3 = x->oldcoeff[3];
#endif
		
		x->freecoeff = x->oldcoeff;
		x->oldcoeff = mycoeff;		
	}
	return (w+5);
}
*/
void peqbank_perform64_fast(t_peqbank *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam)
{
    do_peqbank_perform64_fast(x, dsp64, ins, numins, outs, numouts, sampleframes, flags, x->coeff);
    
    // We still have to shuffle the coeff pointers around.
    
    if (x->coeff != x->oldcoeff) {
        if (x->freecoeff != 0) {
            object_post((t_object *)x, "peqbank~: disaster (fast)!  freecoeff should be zero now!");
        }
        x->freecoeff = x->oldcoeff;
        x->oldcoeff = x->coeff;		
    }
}
/*
t_int *peqbank_perform_fast(t_int *w) {
	t_peqbank *x = (t_peqbank *)(w[3]);
	t_int *result;

	result = do_peqbank_perform_fast(w, x->coeff);
	
	// We still have to shuffle the coeff pointers around.
	
	if (x->coeff != x->oldcoeff) {
		if (x->freecoeff != 0) {
			object_post((t_object *)x, "peqbank~: disaster (fast)!  freecoeff should be zero now!");
		}
		x->freecoeff = x->oldcoeff;
		x->oldcoeff = x->coeff;		
	}
	
	return result;	
}
*/
void do_peqbank_perform64_fast(t_peqbank *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, double *mycoeff)
{
    //t_float *in  = (t_float *)(w[1]);
    //t_float *out = (t_float *)(w[2]);
    //t_peqbank *x = (t_peqbank *)(w[3]);
    //int n        = (int)(w[4]);
    double *in = ins[0];
    double *out = outs[0];
    int n = sampleframes;
    
    // Biquad with linear interpolation: smooth-biquad~
    int i, j, k=0;
    double a0, a1, a2, b1, b2;
    double xn, yn, xm2, xm1, ym2, ym1;
    
    // First copy input vector to output vector (below we'll filter the output in-place
    if (out != in) {
        for (i=0; i<n; ++i) out[i] = in[i];
    }
    
    // Cascade of Biquads
    for (j=x->b_start; j<(x->b_nbpeq+1)*NBCOEFF; j+=NBCOEFF) {
        
        xm2 = x->b_xm2[k];
        xm1 = x->b_xm1[k];
        ym2 = x->b_ym2[k];
        ym1 = x->b_ym1[k];
        
        a0 = mycoeff[j  ];
        a1 = mycoeff[j+1];
        a2 = mycoeff[j+2];
        b1 = mycoeff[j+3];
        b2 = mycoeff[j+4];
        
        for (i=0; i<n; i++) {
            xn = out[i];
            out[i] = yn = (a0 * xn) + (a1 * xm1) + (a2 * xm2) - (b1 * ym1) - (b2 * ym2);
            
            xm2 = xm1;
            xm1 = xn;
            ym2 = ym1;
            ym1 = yn;
        }
        
        x->b_xm2[k] = FLUSH_TO_ZERO(xm2);
        x->b_xm1[k] = FLUSH_TO_ZERO(xm1);
        x->b_ym2[k] = FLUSH_TO_ZERO(ym2);
        x->b_ym1[k] = FLUSH_TO_ZERO(ym1);		
        k ++;
        
    } // cascade loop
}
/*
t_int *do_peqbank_perform_fast(t_int *w, float *mycoeff) {
	t_float *in  = (t_float *)(w[1]);
	t_float *out = (t_float *)(w[2]);
	t_peqbank *x = (t_peqbank *)(w[3]);
	int n        = (int)(w[4]);	

	// Biquad with linear interpolation: smooth-biquad~
	int i, j, k=0;
	float a0, a1, a2, b1, b2;
	float xn, yn, xm2, xm1, ym2, ym1; 
	
	// First copy input vector to output vector (below we'll filter the output in-place
	if (out != in) {
		for (i=0; i<n; ++i) out[i] = in[i];
	}

	// Cascade of Biquads
	for (j=x->b_start; j<(x->b_nbpeq+1)*NBCOEFF; j+=NBCOEFF) {
		
		xm2 = x->b_xm2[k]; 
		xm1 = x->b_xm1[k];
		ym2 = x->b_ym2[k]; 
		ym1 = x->b_ym1[k];	
		
		a0 = mycoeff[j  ];
		a1 = mycoeff[j+1];
		a2 = mycoeff[j+2];
		b1 = mycoeff[j+3];	
		b2 = mycoeff[j+4];
				
		for (i=0; i<n; i++) {
			xn = out[i];
		    out[i] = yn = (a0 * xn) + (a1 * xm1) + (a2 * xm2) - (b1 * ym1) - (b2 * ym2);

			xm2 = xm1;
			xm1 = xn;
			ym2 = ym1;
			ym1 = yn;
		}
		
		x->b_xm2[k] = FLUSH_TO_ZERO(xm2);
		x->b_xm1[k] = FLUSH_TO_ZERO(xm1);
		x->b_ym2[k] = FLUSH_TO_ZERO(ym2);
		x->b_ym1[k] = FLUSH_TO_ZERO(ym1);		
		k ++;
		
	} // cascade loop

	return (w+5);
}
*/
void peqbank_perform64_fast_multi(t_peqbank *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam)
{
    do_peqbank_perform64_fast_multi(x, dsp64, ins, numins, outs, numouts, sampleframes, flags, x->coeff);
    
    /* We still have to shuffle the coeff pointers around. */
    
    if (x->coeff != x->oldcoeff) {
        if (x->freecoeff != 0) {
            object_post((t_object *)x, "peqbank~: disaster (fast)!  freecoeff should be zero now!");
        }
        x->freecoeff = x->oldcoeff;
        x->oldcoeff = x->coeff;		
    }
}
/*
t_int *peqbank_perform_fast_multi(t_int *w) {

	t_peqbank *x = (t_peqbank *)(w[1]);
    
	do_peqbank_perform_fast_multi(x, x->coeff);
	
	// We still have to shuffle the coeff pointers around.
	
	if (x->coeff != x->oldcoeff) {
		if (x->freecoeff != 0) {
			object_post((t_object *)x, "peqbank~: disaster (fast)!  freecoeff should be zero now!");
		}
		x->freecoeff = x->oldcoeff;
		x->oldcoeff = x->coeff;		
	}
	
	return w + 2;	
}
*/
void do_peqbank_perform64_fast_multi(t_peqbank *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, double *mycoeff)
{
    int n = sampleframes;
    
    int i, j, k=0, c;
    double a0, a1, a2, b1, b2;
    
    double xn[x->b_channels], yn[x->b_channels], xm2[x->b_channels], xm1[x->b_channels], ym2[x->b_channels], ym1[x->b_channels];
    
    // First copy input vector to output vector, we'll filter the output in-place
    for (c = 0; c < x->b_channels; c++) {
        if(outs[c] != ins[c]) {
            for (i=0; i<n; i++) {
                outs[c][i] = ins[c][i];
            }
        }
    }
    
    /* Cascade of Biquads */
    for (j=x->b_start; j < (x->b_nbpeq+1)*NBCOEFF; j+=NBCOEFF) {
        
        for(c = 0; c < x->b_channels; c++) {
            xm2[c] = x->b_xm2[k*x->b_channels + c];
            xm1[c] = x->b_xm1[k*x->b_channels + c];
            ym2[c] = x->b_ym2[k*x->b_channels + c];
            ym1[c] = x->b_ym1[k*x->b_channels + c];
        }
        
        a0 = mycoeff[j  ];
        a1 = mycoeff[j+1];
        a2 = mycoeff[j+2];
        b1 = mycoeff[j+3];
        b2 = mycoeff[j+4];
        
        for (i=0; i < n; i++) {
            
#pragma ivdep
            for(c = 0; c < x->b_channels; c++) {
                xn[c] = outs[c][i];
            }
            
#pragma ivdep
#pragma loop_count min(2) max(1000) avg(20)
            for(c = 0; c < x->b_channels; c++) {
                yn[c] = (a0 * xn[c]) + (a1 * xm1[c]) + (a2 * xm2[c]) - (b1 * ym1[c]) - (b2 * ym2[c]);
            }
            
#pragma ivdep
            for(c = 0; c < x->b_channels; c++) {
                outs[c][i] = yn[c];
            }
            
#pragma ivdep
#pragma loop_count min(2) max(1000) avg(20)
            for(c = 0; c < x->b_channels; c++) {
                xm2[c] = xm1[c];
                xm1[c] = xn[c];
                ym2[c] = ym1[c];
                ym1[c] = yn[c];
            }
        }
        
        for(c = 0; c < x->b_channels; c++) {
            x->b_xm2[k*x->b_channels + c] = FLUSH_TO_ZERO(xm2[c]);
            x->b_xm1[k*x->b_channels + c] = FLUSH_TO_ZERO(xm1[c]);
            x->b_ym2[k*x->b_channels + c] = FLUSH_TO_ZERO(ym2[c]);
            x->b_ym1[k*x->b_channels + c] = FLUSH_TO_ZERO(ym1[c]);		
        }
        
        k ++;
        
    } // cascade loop
}
/*
void do_peqbank_perform_fast_multi(t_peqbank *x, float *mycoeff) {

	int n = x->s_n;
    
	int i, j, k=0, c;
	float a0, a1, a2, b1, b2;
    
	float xn[x->b_channels], yn[x->b_channels], xm2[x->b_channels], xm1[x->b_channels], ym2[x->b_channels], ym1[x->b_channels]; 

	// First copy input vector to output vector, we'll filter the output in-place
    for (c = 0; c < x->b_channels; c++) {
        if(x->s_vec_out[c] != x->s_vec_in[c]) {
            for (i=0; i<n; i++) {
                x->s_vec_out[c][i] = x->s_vec_in[c][i];
            }
        }
    }
    
	// Cascade of Biquads
	for (j=x->b_start; j < (x->b_nbpeq+1)*NBCOEFF; j+=NBCOEFF) {

		for(c = 0; c < x->b_channels; c++) {
            xm2[c] = x->b_xm2[k*x->b_channels + c]; 
            xm1[c] = x->b_xm1[k*x->b_channels + c];
            ym2[c] = x->b_ym2[k*x->b_channels + c]; 
            ym1[c] = x->b_ym1[k*x->b_channels + c];	
		}
        
		a0 = mycoeff[j  ];
		a1 = mycoeff[j+1];
		a2 = mycoeff[j+2];
		b1 = mycoeff[j+3];	
		b2 = mycoeff[j+4];
        
		for (i=0; i < n; i++) {

#pragma ivdep
            for(c = 0; c < x->b_channels; c++) {
                xn[c] = x->s_vec_out[c][i];
			}
			
#pragma ivdep
#pragma loop_count min(2) max(1000) avg(20)
			for(c = 0; c < x->b_channels; c++) {
                yn[c] = (a0 * xn[c]) + (a1 * xm1[c]) + (a2 * xm2[c]) - (b1 * ym1[c]) - (b2 * ym2[c]);
			}
			
#pragma ivdep
			for(c = 0; c < x->b_channels; c++) {
				x->s_vec_out[c][i] = yn[c];
			}
			
#pragma ivdep
#pragma loop_count min(2) max(1000) avg(20)
			for(c = 0; c < x->b_channels; c++) {
                xm2[c] = xm1[c];
                xm1[c] = xn[c];
                ym2[c] = ym1[c];
                ym1[c] = yn[c];
            }
		}
		
		for(c = 0; c < x->b_channels; c++) {
            x->b_xm2[k*x->b_channels + c] = FLUSH_TO_ZERO(xm2[c]);
            x->b_xm1[k*x->b_channels + c] = FLUSH_TO_ZERO(xm1[c]);
            x->b_ym2[k*x->b_channels + c] = FLUSH_TO_ZERO(ym2[c]);
            x->b_ym1[k*x->b_channels + c] = FLUSH_TO_ZERO(ym1[c]);		
        }
        
		k ++;
		
	} // cascade loop
}
*/

void peqbank_clear(t_peqbank *x) {

	int i;
	
	for(i=0; i < (x->b_max * x->b_channels); ++i) {
		x->b_ym1[i] = 0.0;
		x->b_ym2[i] = 0.0;
		x->b_xm1[i] = 0.0;
		x->b_xm2[i] = 0.0;
	}
}

void peqbank_dsp64(t_peqbank *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags)
{
    //int i;
    
    x->b_Fs = samplerate;
    
    peqbank_clear(x);
    
    if(x->b_channels == 1) {
        if (x->b_version == FAST) {
            object_method(dsp64, gensym("dsp_add64"), x, peqbank_perform64_fast, 0, NULL);
        } else {
            object_method(dsp64, gensym("dsp_add64"), x, peqbank_perform64_smooth, 0, NULL);
        }
    } else {
        /*
        for(i = 0; i < x->b_channels; i++) {
            x->s_vec_in[i] = sp[i]->s_vec;
            x->s_vec_out[i] = sp[i + x->b_channels]->s_vec;
        }
        x->s_n = sp[0]->s_n;
         */
        if (x->b_version == FAST) {
            object_method(dsp64, gensym("dsp_add64"), x, peqbank_perform64_fast_multi, 0, NULL);
        } else {
            object_method(dsp64, gensym("dsp_add64"), x, peqbank_perform64_smooth_multi, 0, NULL);
        }
        
    }
}
/*
void peqbank_dsp(t_peqbank *x, t_signal **sp, short *connect) {

    int i;
    
	x->b_Fs = sp[0]->s_sr;
    
	peqbank_clear(x);

    if(x->b_channels == 1) {
        if (x->b_version == FAST) {
            dsp_add(peqbank_perform_fast, 4, sp[0]->s_vec, sp[1]->s_vec, x, sp[0]->s_n);
        } else {
            dsp_add(peqbank_perform_smooth, 4, sp[0]->s_vec, sp[1]->s_vec, x, sp[0]->s_n);
        }
    } else {
        for(i = 0; i < x->b_channels; i++) {
            x->s_vec_in[i] = sp[i]->s_vec;
            x->s_vec_out[i] = sp[i + x->b_channels]->s_vec;
        }
        x->s_n = sp[0]->s_n;
        if (x->b_version == FAST) {
            dsp_add(peqbank_perform_fast_multi, 1, x);
        } else {
            dsp_add(peqbank_perform_smooth_multi, 1, x);
        }
        
    }
        
}
 */

void peqbank_reset(t_peqbank *x) {
	x->b_nbpeq = 0;
	x->b_start = NBCOEFF;
	long oldmax = x->b_max;
	x->b_max = MAXELEM;
	if(oldmax != x->b_max){
		peqbank_freemem(x);
		peqbank_allocmem(x);
	}
	
	if(x->param){
		memset(x->param, 0, x->b_max * NBPARAM * sizeof(double));
	}
	if(x->oldparam){
		memset(x->oldparam, 0, x->b_max * NBPARAM * sizeof(double));
	}
	if(x->coeff){
		memset(x->coeff, 0, x->b_max * NBCOEFF * sizeof(double));
	}
	if(x->oldcoeff){
		memset(x->oldcoeff, 0, x->b_max * NBCOEFF * sizeof(double));
	}
	if(x->newcoeff){
		memset(x->newcoeff, 0, x->b_max * NBCOEFF * sizeof(double));
	}
	if(x->freecoeff){
		memset(x->freecoeff, 0, x->b_max * NBCOEFF * sizeof(double));
	}
	if(x->b_ym1){
		memset(x->b_ym1, 0, x->b_max * x->b_channels * sizeof(double));
	}
	if(x->b_ym2){
		memset(x->b_ym2, 0, x->b_max * x->b_channels * sizeof(double));
	}
	if(x->b_xm1){
		memset(x->b_xm1, 0, x->b_max * x->b_channels * sizeof(double));
	}
	if(x->b_xm2){
		memset(x->b_xm2, 0, x->b_max * x->b_channels * sizeof(double));
	}
	if(x->myList){
		memset(x->myList, 0, x->b_max * NBCOEFF * sizeof(t_atom));
	}

	//peqbank_free(x);
    peqbank_init(x);
	peqbank_compute(x);	
}

int maxelem(t_peqbank *x, t_symbol *s, short argc, t_atom *argv, int rest) {

	x->b_nbpeq = 0;
	x->b_start = NBCOEFF;
	 		
	if ((rest < argc) && (argv[rest].a_type == A_LONG)) { 
		x->b_max = argv[rest].a_w.w_long;
		if (x->b_max < 2) {
			x->b_max = 2;
		}
		rest ++;
	}
	
	peqbank_freemem(x);
	peqbank_allocmem(x);
	peqbank_init(x);
	//peqbank_reset(x);
		
	return(rest);		
}

int shelf(t_peqbank *x, t_symbol *s, short argc, t_atom *argv, int rest) {
	if ((rest+4 < argc) && (argv[rest  ].a_type == A_FLOAT) && (argv[rest+1].a_type == A_FLOAT) 
						&& (argv[rest+2].a_type == A_FLOAT) && (argv[rest+3].a_type == A_FLOAT)
						&& (argv[rest+4].a_type == A_FLOAT)) {
						
		x->param[0] = argv[rest  ].a_w.w_float;
		x->param[1] = argv[rest+1].a_w.w_float;
		x->param[2] = argv[rest+2].a_w.w_float;
		x->param[3] = argv[rest+3].a_w.w_float;
		x->param[4] = argv[rest+4].a_w.w_float;
		
		if (x->param[3] == 0) x->param[3] = SMALL;
		if (x->param[4] == 0) x->param[4] = SMALL;

		rest += 5;
		x->b_start = 0;
	} 
	return(rest);
}

int peq(t_peqbank *x, t_symbol *s, short argc, t_atom *argv, int rest) {

	int index=NBPARAM;
	double G0, G, GB;
		
	x->b_nbpeq = 0;
					
	while ((rest < argc) && (x->b_nbpeq+1 < x->b_max)) {
		if ((rest+4 < argc) && (argv[rest  ].a_type == A_FLOAT) && (argv[rest+1].a_type == A_FLOAT) 
							&& (argv[rest+2].a_type == A_FLOAT) && (argv[rest+3].a_type == A_FLOAT) 
							&& (argv[rest+4].a_type == A_FLOAT)) {
			
			x->param[index  ] = argv[rest  ].a_w.w_float;
			x->param[index+1] = argv[rest+1].a_w.w_float;
			G0 = argv[rest+2].a_w.w_float;
			G  = argv[rest+3].a_w.w_float;
			GB = argv[rest+4].a_w.w_float;

			if (x->param[index] == 0) x->param[index] = SMALL;
							
			if (G0 == G) {
				GB = G0;
				G  = G0 + SMALL;
				G0 = G0 - SMALL;
			} else if (!((G0 < GB) && (GB < G)) && !((G0 > GB) && (GB > G))) GB = (G0 + G) * 0.5f; 
						
			x->param[index+2] = G0;
			x->param[index+3] = G;
			x->param[index+4] = GB;
			
			index += NBPARAM;
			rest  += NBPARAM;
			x->b_nbpeq ++;
			
			if (argv[rest].a_type != A_FLOAT) {
				if (argv[rest].a_w.w_sym == ps_peq) rest ++;
				else return(rest);
			} // End of if
		} // End of if
	} // End of while
	return(rest);
}

void peqbank_maxelem(t_peqbank *x, t_symbol *s, short argc, t_atom *argv) {
	maxelem(x,s,argc,argv,0);
	peqbank_compute(x);
}

void peqbank_shelf(t_peqbank *x, t_symbol *s, short argc, t_atom *argv) {
	shelf(x,s,argc,argv,0);
	peqbank_compute(x);
}

void peqbank_peq(t_peqbank *x, t_symbol *s, short argc, t_atom *argv) {
	peq(x,s,argc,argv,0);
	peqbank_compute(x);
}

void peqbank_fast(t_peqbank *x, t_symbol *s, short argc, t_atom *argv) {
     x->b_version = FAST;
}

void peqbank_smooth(t_peqbank *x, t_symbol *s, short argc, t_atom *argv) {
     x->b_version = SMOOTH;
}

void peqbank_list(t_peqbank *x, t_symbol *s, short argc, t_atom *argv) {

	int rest=0;

	x->b_Fs = sys_getsr();
	x->b_nbpeq = 0;
	x->b_start = NBCOEFF;

    /* process list of atoms */
	while ((rest < argc) && (x->b_nbpeq+1 < x->b_max)) {
		if (argv[rest].a_type != A_SYM) return;
		if (argv[rest].a_w.w_sym == ps_maxelem) {
			if (argv[rest+1].a_w.w_long != x->b_max) 
				rest = maxelem(x, s, argc, argv, rest+1);
			else rest += 2;
		}									
		if (argv[rest].a_w.w_sym == ps_shelf) rest = shelf(x, s, argc, argv, rest+1);
		if (argv[rest].a_w.w_sym == ps_peq) rest = peq(x, s, argc, argv, rest+1);
		if (argv[rest].a_w.w_sym == ps_fast) { 
			peqbank_fast(x, s, argc, argv); 
			rest ++;
		}
		if (argv[rest].a_w.w_sym == ps_smooth) {
            peqbank_smooth(x, s, argc, argv);
            rest ++;
        }
		if (argv[rest].a_w.w_sym == ps_channels) {
            rest += 2;
        }
	}	
	peqbank_compute(x);
}

#define ASFLOAT(x) (((x).a_type == A_FLOAT) ? ((x).a_w.w_float) : ((float) (x).a_w.w_long))

void peqbank_biquads(t_peqbank *x, t_symbol *s, short argc, t_atom *argv) {
	int i;
	
	for (i = 0; i < argc; ++i) {
		if (argv[i].a_type == A_SYM) {
			object_error((t_object *)x, "peqbank~: all arguments to biquads message must be numbers");
			return;
		}
	}
	
	if ((argc % 5) != 0) {
		object_error((t_object *)x, "peqbank~: biquads message must have a multiple of 5 arguments");
		return;
	}
	
	if ((argc / 5) > x->b_max) {
		object_error((t_object *)x, "peqbank~: Too many biquad coefficients (only memory for %d filters)", x->b_max);
		object_post((t_object *)x, "   (ignoring entire biquads list)");
		return;
	}
	
	for (i = 0; i < argc; ++i) {
		x->newcoeff[i] = ASFLOAT(argv[i]);
	}


	/* These should happen atomically... */
	x->b_start = 0;
	x->b_nbpeq = (argc/5)-1;    /* The first biquad is the "shelf"; the other n-1 are the "peq"s */
	swap_in_new_coeffs(x);	
}		
		
void peqbank_cheby(t_peqbank *x, t_symbol *s, short argc, t_atom *argv) {
	
	double freq;
	int order;
	double ripple;
	
	int i;
	
	for (i = 0; i < argc; ++i) {
		if (argv[i].a_type == A_SYM) {
			object_error((t_object *)x, "peqbank~: all arguments to highpass message must be numbers");
			return;
		}
	}
	
	if (argc == 0) {
		object_error((t_object *)x, "peqbank~: message must have at least one argument");
		return;
	}
	
	if (argc > 3) {
		object_error((t_object *)x, "peqbank~: message must not more than three arguments");
		return;
	}
	
	freq = 1000.;
	
	if(argc > 0) {
		freq = atom_getfloatarg(0, argc, argv);
	}
	if(argc > 1) {
		order = atom_getintarg(1, argc, argv);
	} else {
		order = 8;
	}
	
	if(order % 2 != 0 || order > 8) {
		object_post((t_object *)x, "peqbank~: bad order, must be even number 2-8");
	}
	
	if(argc > 2) {
		ripple = atom_getfloatarg(1, argc, argv);
	} else {
		ripple = 0.8;
	}
	
	if (order/2 > x->b_max) {
		object_error((t_object *)x, "peqbank~: not enough space for cascade (only memory for %d filters)", x->b_max);
		return;
	}
	
	if(s == ps_highpass) {
		if(peqbank_cheby_coeffs(x, freq, 1, order, ripple) != 0) {
			object_post((t_object *)x, "peqbank~: error calculating cheby filter coefficients");
		}
	} else {
		if(peqbank_cheby_coeffs(x, freq, 0, order, ripple) != 0) {
			object_post((t_object *)x, "peqbank~: error calculating cheby filter coefficients");
		}
	}
}	

int peqbank_cheby_coeffs(t_peqbank *x, double freq, int type, int order, double ripple) {

	int error=0;

	double FC, PR;
	double A0, A1, A2, B1, B2;
	double RP, IP, ES, VX, KX, T, W, M, D, K, X0, X1, X2, Y1, Y2;
	double GAIN;
	unsigned int LH, NP, P;

	FC = freq;
	LH = type;
	PR = ripple;
	NP = order;
	
	// sanity check input parameters
	if ((NP < 2) || (NP > 8) || (NP % 2))
	{
		object_post((t_object *)x, "peqbank~: cheby: invalid number of poles as argument, must be an even number between 2 and 8");
		error = 1;
	}
	
	if ((LH != 1) && (LH != 0))
	{
		object_post((t_object *)x, "peqbank~: cheby: lowpass/highpass selector out of range (must be 1 or 0)");
		error = 1;
	}
	
	FC = FC / sys_getsr();
	if (FC > 0.5f)
	{
		object_post((t_object *)x, "peqbank~: cheby: cutoff frequency out of range (fs = %f)", sys_getsr());
		error = 1;
	}
	
	if ((PR < 0) || (PR > 29))
	{
		object_post((t_object *)x, "peqbank~: cheby: percent ripple out of range");
		error = 1;
	}
	
	if (error)
	{
		
	class_register(CLASS_BOX, peqbank_class);
	return 0;
}
	else
	{
		//	fprintf(stderr, "cheby: calculating biquad coefficients, FC = %f\n",FC);
		for (P=0;P<NP/2;P++)
		{
			// fprintf(stderr, "cheby: calculating biquad coefficients\n");
			// calculate pole location on unit circle
			RP = -cos(PI/(NP*2) + P * PI/NP);
			IP =  sin(PI/(NP*2) + P * PI/NP);
			// fprintf(stderr, "cheby: RP = %f IP = %f\n", RP, IP);
			
			// warp from a circle to an ellipse
			if (PR != 0)
			{
				ES = sqrt(((100.0/(100.0-PR))*(100.0/(100.0-PR)))-1.0);
				VX = (1.0/NP) * log((1.0/ES) + sqrt((1.0/(ES*ES))+1.0));
				KX = (1.0/NP) * log((1.0/ES) + sqrt((1.0/(ES*ES))-1.0));
				KX = (exp(KX) + exp(-KX))/2.0;
				RP = RP * ((exp(VX) - exp(-VX))/2.0)/KX;
				IP = IP * ((exp(VX) + exp(-VX))/2.0)/KX;
			}
			// fprintf(stderr, "cheby: after warping RP = %f IP = %f\n", RP, IP);
			// fprintf(stderr, "cheby: ES = %f VX = %f KX = %f\n", ES, VX, KX);
			
			// s-domain to z-domain conversion
			T  = 2 * tan(0.5);
			W  = 2*PI*FC;
			M  = (RP*RP) + (IP*IP);
			D  = 4 - 4*RP*T + M*(T*T); 
			X0 = (T*T)/D;
			X1 = 2*(T*T)/D;
			X2 = (T*T)/D;
			Y1 = (8 - 2*M*(T*T))/D;
			Y2 = (-4 - 4*RP*T - M*(T*T))/D;
			
			// lopass to lopass or lopass to hipass transform
			if (LH == 1)
				K = -cos(W/2.0 + 0.5) / cos(W/2.0 - 0.5);
			if (LH == 0)
				K =  sin(0.5 - W/2.0) / sin(0.5 + W/2.0);
			
			D  = 1.0 + Y1*K - Y2*(K*K);
			A0 = (X0 - X1*K + X2*(K*K))/D;
			A1 = (-2*X0*K + X1 + X1*(K*K) - 2*X2*K)/D;
			A2 = (X0*(K*K) - X1*K + X2)/D;
			B1 = (2*K + Y1 + Y1*(K*K) - 2*Y2*K)/D;
			B2 = (-(K*K) - Y1*K + Y2)/D;
			GAIN = (1.0 - (B1 + B2)) / (A0 + A1 + A2);
			if (LH == 1)
			{
				A1 = -A1;
				B1 = -B1;
			}
			
			// fprintf(stderr, "coeffs: %f %f %f %f %f : gain %f\n",A0*GAIN,A1*GAIN,A2*GAIN,B1*GAIN,B2*GAIN, GAIN);
			
			x->newcoeff[(P*5)]   = A0 * GAIN;
			x->newcoeff[(P*5)+1] = A1 * GAIN;
			x->newcoeff[(P*5)+2] = A2 * GAIN;
			x->newcoeff[(P*5)+3] = -B1;
			x->newcoeff[(P*5)+4] = -B2;
		}
		
		/* These should happen atomically... */
		x->b_start = 0;
		x->b_nbpeq = P-1;    /* The first biquad is the "shelf"; the other n-1 are the "peq"s */
		swap_in_new_coeffs(x);
		
		return 0;
	}
}

void peqbank_init(t_peqbank *x) {
	// memory allocation has been moved to peqbank_allocmem() --JM

	int i;

	for (i=0; i<x->b_max * NBPARAM; ++i) {
		x->param[i]    = 0.0;
		x->oldparam[i] = 0.0;
		x->coeff[i]    = 0.0;
	}
	
	for (i=0; i < (x->b_max * x->b_channels); ++i) {
		x->b_ym1[i] = 0.0;
		x->b_ym2[i] = 0.0;
		x->b_xm1[i] = 0.0;
		x->b_xm2[i] = 0.0;
	}
	
#ifdef DEBUG
	x->testcoeff3 = 0.0;
#endif
}

void peqbank_assist(t_peqbank *x, void *b, long m, long a, char *s) 

/*{

	assist_string(RES_ID, m, a, 1, 2, s);
}*/

{
	if (m == ASSIST_INLET)
		sprintf(s,"(signal)input");
	else {
        if(a == x->b_channels) {
			sprintf(s,"(list)shelf + parametric EQ biquad coefficients [a0 a1 a2 b1 b2]*");
        } else {
			sprintf(s,"(signal) output");
        }
	}
}

void *peqbank_new(t_symbol *s, short argc, t_atom *argv) {

	int i, rest=0;
	 	
	t_peqbank *x = (t_peqbank *)object_alloc(peqbank_class);
	if(!x){
		return NULL;
	}
	
	x->b_max = MAXELEM;
    x->b_channels = 1;
    
	/* get the maximum of filters */
	if (argv[0].a_type == A_LONG) x->b_max = argv[0].a_w.w_long;
	else for (i=0; i<argc; ++i) {	
        if (argv[rest].a_w.w_sym == ps_maxelem) {
            rest ++;
            if ((rest < argc) && (argv[rest].a_type == A_LONG)) { 
                x->b_max = argv[rest].a_w.w_long;
                if (x->b_max < 2) {
                    x->b_max = 2;
                }
                break;
            }
        }	
        if (argv[rest].a_w.w_sym == ps_channels) {
            rest ++;
            if ((rest < argc) && (argv[rest].a_type == A_LONG)) { 
                x->b_channels = argv[rest].a_w.w_long;
                if (x->b_channels < 1) {
                    x->b_channels = 1;
                }
                break;
            }
        }	
    }

	x->already_peqbank_compute = 0;
	x->need_to_recompute = 0;
	peqbank_allocmem(x);
	peqbank_init(x);
		
    dsp_setup((t_pxobject *)x, x->b_channels);			// number of inlets
    if(x->b_channels > 1) {  // inplace processing is broken for multichannel, msp inverts the order of the output channel buffers (as of 08/28/2009) -aws
        x->b_obj.z_misc = Z_NO_INPLACE;
    }
    x->b_outlet = listout((t_object *)x);	// Create an outlet
    for(i = 0; i < x->b_channels; i++) {
        outlet_new((t_object *)x, "signal");	// type of outlet: "signal"
    }
	x->b_version = SMOOTH;
	peqbank_clear(x);
    peqbank_list(x, s, argc, argv);
	
    return (x);
}

void peqbank_allocmem(t_peqbank *x){
	/* alocate and initialize memory */
	x->param    = (double*) sysmem_newptr( x->b_max * NBPARAM * sizeof(*x->param) );
	x->oldparam = (double*) sysmem_newptr( x->b_max * NBPARAM * sizeof(*x->oldparam) );
	x->coeff    = (double*) sysmem_newptr( x->b_max * NBCOEFF * sizeof(*x->coeff) );
	x->oldcoeff = x->coeff;
	x->newcoeff = (double*) sysmem_newptr( x->b_max * NBCOEFF * sizeof(*x->newcoeff) );
	x->freecoeff = (double*) sysmem_newptr( x->b_max * NBCOEFF * sizeof(*x->freecoeff) );
	x->b_ym1    = (double*) sysmem_newptr( x->b_max * x->b_channels * sizeof(*x->b_ym1) );
	x->b_ym2    = (double*) sysmem_newptr( x->b_max * x->b_channels * sizeof(*x->b_ym2) );     
	x->b_xm1    = (double*) sysmem_newptr( x->b_max * x->b_channels * sizeof(*x->b_xm1) );
	x->b_xm2    = (double*) sysmem_newptr( x->b_max * x->b_channels * sizeof(*x->b_xm2) );     
    //x->s_vec_in = (t_float**) sysmem_newptr( x->b_channels * sizeof(t_float*));
    //x->s_vec_out = (t_float**) sysmem_newptr( x->b_channels * sizeof(t_float*));
	x->myList   = (t_atom*)  sysmem_newptr( x->b_max * NBCOEFF * sizeof(*x->myList) );     

	if (x->param == NIL || x->oldparam == NIL || x->coeff == NIL || x->newcoeff == NIL ||
	    x->freecoeff == NIL || x->b_ym1 == NIL || x->b_ym2 == NIL || x->b_xm1 == NIL || 
	    x->b_xm2 == NIL || x->myList == NIL) {
		object_post((t_object *)x, "peqbank~: warning: not enough memory.  Expect to crash soon.");
	}
}

void peqbank_freemem(t_peqbank *x){
	sysmem_freeptr((char *) x->param);
	sysmem_freeptr((char *) x->oldparam);

	if (x->coeff != x->oldcoeff) sysmem_freeptr((char *) x->oldcoeff);
	sysmem_freeptr((char *) x->coeff);
	sysmem_freeptr((char *) x->newcoeff);
	if (x->freecoeff) sysmem_freeptr((char *) x->freecoeff);

	sysmem_freeptr((char *) x->b_ym1);
	sysmem_freeptr((char *) x->b_ym2);
	sysmem_freeptr((char *) x->b_xm1);
	sysmem_freeptr((char *) x->b_xm2);
    //sysmem_freeptr((char *) x->s_vec_in);
    //sysmem_freeptr((char *) x->s_vec_out);
	sysmem_freeptr((char *) x->myList);
}

void  peqbank_free(t_peqbank *x) {
	dsp_free(&(x->b_obj));
	peqbank_freemem(x);
}

#define WORRIED_ABOUT_PEQBANK_REENTRANCY

void peqbank_compute(t_peqbank *x) {		
	int i;
	
#ifdef WORRIED_ABOUT_PEQBANK_REENTRANCY
	if (x->already_peqbank_compute) {
	  x->need_to_recompute = 1;
	  object_post((t_object *)x, "Still computing filter design from the previous message; ignoring new message.");
	  return;
	}
	/* If we get interrupted right here then we're screwed. */
	x->already_peqbank_compute = 1;
#endif

 startover:
	// Do the actual computation of coefficients, into x->newcoeff
	compute_shelf(x);
	for (i=NBPARAM; i<(x->b_nbpeq+1)*NBPARAM; i+=NBPARAM) compute_parameq(x,i);

	if (x->need_to_recompute) {
	  // This procedure was called while the previous invocation
	  // was still running, so the coefficients we just computed
	  // are junk.
	  x->need_to_recompute = 0;
	  goto startover;
	}

	swap_in_new_coeffs(x);

#ifdef WORRIED_ABOUT_PEQBANK_REENTRANCY
	x->already_peqbank_compute = 0;
#endif
}


void swap_in_new_coeffs(t_peqbank *x) {
	double *prevcoeffs, *prevnew, *prevfree;
	int i;

	// To make the new coefficients take effect we swap around the pointers to the
	// coefficient buffers.  See the large comment at the top of this file.
	// The audio processing interrupt might come at any time.
	
	if (x->coeff == x->oldcoeff) {
		// Normal case: these are the first new coeffients since the last perform routine		

#ifdef DEBUG		
		if (!test_normal_state(x)) post("peqbank_compute: x->coeff == x->oldcoeff");
#endif

		if (x->freecoeff == 0) {
			object_post((t_object *)x, "peqbank: disaster!  freecoeff shouldn't be zero here!");
			   post("  coeff = %p, oldcoeff = %p, newcoeff = %p, freecoeff = %p",
   					x->coeff, x->oldcoeff, x->newcoeff, x->freecoeff);

		}
		prevcoeffs = x->coeff;
		prevnew = x->newcoeff;
		prevfree = x->freecoeff;
		
		x->freecoeff = 0;
		x->coeff = x->newcoeff;	// Now if we're interrupted the new values will be used.
		x->newcoeff = prevfree;

#ifdef DEBUG		
		if (!test_newcoeffs_state(x)) post("peqbank_compute: just set first new coeffs");
#endif

	} else {
		// We already computed new coeffients since the last perform routine, and now we
		// have even newer ones.  The perform routine may have interrupted this procedure
		// already, but it only would have changed oldcoeffs and freecoeffs.

#ifdef DEBUG		
		if (!test_newcoeffs_state(x)) post("peqbank_compute: x->coeff != x->oldcoeff");
#endif
		
		prevcoeffs = x->coeff;
		prevnew = x->newcoeff;
		
		x->coeff = x->newcoeff;
		x->newcoeff = prevcoeffs;

#ifdef DEBUG		
		if (!test_newcoeffs_state(x)) post("peqbank_compute: just set new new coeffs");
#endif
	}

	// Output the new coefficients out the outlet
	for (i=0; i<(x->b_nbpeq+1)*NBPARAM; i++) atom_setfloat(x->myList+i, x->coeff[i]);		
	outlet_list(x->b_outlet, 0L, (x->b_nbpeq+1)*NBPARAM, x->myList);
}



void compute_parameq(t_peqbank *x, int index) {
	
	/* Biquad coefficient estimation */
	double G0 = pow10(x->param[index+2] * 0.05);
	double G  = pow10(x->param[index+3] * 0.05);
	double GB = pow10(x->param[index+4] * 0.05);
	
	double w0  = TWOPI * x->param[index] / x->b_Fs;
	double G02 = G0 * G0;
	double GB2 = GB * GB;
	double G2  = G * G;	
	double w02 = w0 * w0;
	
	double val1 = 1.0 / fabs(G2 - GB2);
	double val2 = fabs(G2 - G02);
	double val3 = fabs(GB2 - G02);
	double val4 = (w02 - PI2) * (w02 - PI2);
	
	double mul1 = LOG_22 * x->param[index+1];
	double Dw   = 2.0 * w0 * sinh(mul1);
	double mul2 = val3 * PI2 * Dw * Dw;
	double num  = G02 * val4 + G2 * mul2 * val1;
	double den  = val4 + mul2 * val1;
	
	double G1  = sqrt(num / den);
	double G12 = G1 * G1;
	
	double mul3 = G0 * G1;
	double val5 = fabs(G2 - mul3);
	double val6 = fabs(G2 - G12);
	double val7 = fabs(GB2 - mul3);
	double val8 = fabs(GB2 - G12);	
	double val9 = sqrt((val3 * val6) / (val8 * val2));
	
	double tan0 = tan(w0 * 0.5);
	double w1   = w0 * pow2(x->param[index+1] * -0.5);
	double tan1 = tan(w1 * 0.5);
	double tan2 = val9 * tan0 * tan0 / tan1;
		
	double W2 = sqrt(val6 / val2) * tan0 * tan0;
	double DW = tan2 - tan1;
	
	double C = val8 * DW * DW - 2.0 * W2 * (val7 - sqrt(val3 * val8));
	double D = 2.0 * W2 * (val5 - sqrt(val2 * val6));
	double A = sqrt((C + D) * val1);
	double B = sqrt((G2 * C + GB2 * D) * val1);
	
	double val10 = 1.0 / (1.0 + W2 + A);
    
   	/* New values */
 	x->newcoeff[index  ] = (G1 + G0 * W2 + B)     * val10; 
	x->newcoeff[index+1] = -2.0 * (G1 - G0 * W2) * val10;
	x->newcoeff[index+2] = (G1 - B + G0 * W2)     * val10;
	x->newcoeff[index+3] = -2.0 * (1.0 - W2)    * val10;
	x->newcoeff[index+4] = (1.0 + W2 - A)        * val10;
}

void compute_shelf(t_peqbank *x) {
	
	/* Biquad coefficient estimation */	
	double G1 = pow10((x->param[0] - x->param[1]) * 0.05);
	double G2 = pow10((x->param[1] - x->param[2]) * 0.05);
	double Gh = pow10(x->param[2] * 0.05);
	
	/* Low shelf */
	double X  = tan(x->param[3] * PI / x->b_Fs) / sqrt(G1);
	double L1 = (X - 1.0) / (X + 1.0);
	double L2 = (G1 * X - 1.0) / (G1 * X + 1.0);
	double L3 = (G1 * X + 1.0) / (X + 1.0);
	
	/* High shelf */
	double Y  = tan(x->param[4] * PI / x->b_Fs) / sqrt(G2);
	double H1 = (Y - 1.0) / (Y + 1.0);
	double H2 = (G2 * Y - 1.0) / (G2 * Y + 1.0);
	double H3 = (G2 * Y + 1.0) / (Y + 1.0);
	
	double C0 = L3 * H3 * Gh;
  
    /* New values */
 	x->newcoeff[0] = C0; 
	x->newcoeff[1] = C0 * (L2 + H2);
	x->newcoeff[2] = C0 * L2 * H2;
	x->newcoeff[3] = L1 + H1;
	x->newcoeff[4] = L1 * H1;
}

double pow10(double x) {
	return exp(LOG_10 * x);
}

double pow2(double x) {
	return exp(LOG_2 * x);
}


#define YUCKKYIF(test) if (test) {post(" normal:  " #test); ok = 0;}
int test_normal_state(t_peqbank *x) {
	int ok = 1;
	YUCKKYIF (x->coeff == 0)
	YUCKKYIF (x->oldcoeff == 0)
	YUCKKYIF (x->coeff != x->oldcoeff) 
	YUCKKYIF (x->newcoeff == 0) 
	YUCKKYIF (x->freecoeff == 0) 
	YUCKKYIF (x->coeff == x->newcoeff)
	YUCKKYIF (x->coeff == x->freecoeff)
	YUCKKYIF (x->newcoeff == x->freecoeff)

	return ok;
}

int test_newcoeffs_state(t_peqbank *x) {
	int ok = 1;
	YUCKKYIF (x->oldcoeff == 0)
	YUCKKYIF (x->coeff == 0)
	YUCKKYIF (x->newcoeff == 0) 
	YUCKKYIF (x->freecoeff != 0)
	
	YUCKKYIF (x->coeff == x->oldcoeff)
	YUCKKYIF (x->coeff == x->newcoeff)
	YUCKKYIF (x->newcoeff == x->oldcoeff)

	return ok;
}
