/*---------------------------------------------------------------------------*/
/*
  Plugin to calculate the deconvolution of a measured 3d+time dataset
  with a specified input stimulus time series.  Output consists of the
  least squares estimate of the impulse response function, and the fitted 
  time series (input stimulus convolved with estimated impulse response
  function).

  File:    plug_deconvolve.c
  Author:  B. Douglas Ward
  Date:    09 September 1998

*/


/*---------------------------------------------------------------------------*/

#define PROGRAM_NAME "plug_deconvolve"               /* name of this program */
#define PROGRAM_AUTHOR "B. Douglas Ward"                   /* program author */
#define PROGRAM_DATE "09 September 1998"         /* date of last program mod */

#define MAX_NAME_LENGTH 80              /* max. streng length for file names */
#define MAX_ARRAY_SIZE 1000        /* max. number of time series data points */
#define MAX_XVARS 200                           /* max. number of parameters */
#define MAX_STIMTS 10                 /* max. number of stimulus time series */

#define RA_error DC_error

#include "afni.h"
#include "matrix.h"


/*---------------------------------------------------------------------------*/
/*
   Print error message and stop.
*/

void DC_error (char * message)
{
  fprintf (stderr, "%s Error: %s \n", PROGRAM_NAME, message);

}


/*---------------------------------------------------------------------------*/

#ifndef ALLOW_PLUGINS
#  error "Plugins not properly set up -- see machdep.h"
#endif


/*------------- string to 'help' the user -------------*/

static char helpstring[] =
   " Purpose: Control DC_Fit, DC_Err, and DC_IRF  Deconvolution Functions.  \n"
   "                                                                        \n"
   " Control:     Baseline  = 'Constant', 'Linear', or 'Quadratic'          \n"
   "                           Is baseline 'a', 'a+bt', or 'a+bt+ct^2' ?    \n"
   "              NFirst    = Number of first time series point to use.     \n"
   "              NLast     = Number of last time series point to use.      \n"
   "              IRF Label = Which stimulus to use for generating IRF plot.\n"
   "                                                                        \n"
   " Stimulus:    Label     = Name for reference to this input stimulus.    \n"
   "              File      = Time series file representing input stimulus. \n"
   "              Min Lag   = Minimum time delay for impulse response.      \n"
   "              Max Lag   = Maximum time delay for impulse response.      \n"
;


/*------- Strings for baseline control ------*/

#define NBASE 3
static char * baseline_strings[NBASE] = {"Constant", "Linear", "Quadratic"};

/*--------------- prototypes for internal routines ---------------*/

char * DC_main( PLUGIN_interface * ) ;  /* the entry point */

void DC_Fit ();
void DC_Err ();
void DC_IRF ();
void calculate_results();



/*---------------- global data -------------------*/

static PLUGIN_interface * global_plint = NULL;

static int plug_polort=1;   /* degree of polynomial for baseline model */
static int plug_NFirst=0;   /* first image from input 3d+time dataset to use */
static int plug_NLast=1000; /* last image from input 3d+time dataset to use */
static int plug_IRF=-1;     /* which impulse response fuction to plot */
static int initialize=1;    /* flag for perform initialization */

static int num_stimts = 0;                 /* number of stimulus time series */
static char * stim_label[MAX_STIMTS];      /* stimulus time series labels */
static MRI_IMAGE * stimulus[MAX_STIMTS];   /* stimulus time series arrays */
static int min_lag[MAX_STIMTS];  /* minimum time delay for impulse response */
static int max_lag[MAX_STIMTS];  /* maximum time delay for impulse response */

static matrix x;                  /* independent variable X matrix */
static matrix xtxinv;             /* matrix:  1/(X'X)     for full model*/
static matrix xtxinvxt;           /* matrix:  (1/(X'X))X' for full model */
static matrix abase;              /* A matrix for baseline model */
static matrix ardcd[MAX_STIMTS];  /* array of A matrices for reduced models */
static matrix afull;              /* A matrix for full model */


/*---------------------------------------------------------------------------*/
/*
  Include deconvolution analysis software.
*/

#include "Deconvolve.c"


/*---------------------------------------------------------------------------*/
/*
   Set up the interface to the user.
*/

PLUGIN_interface * PLUGIN_init( int ncall )
{
   PLUGIN_interface * plint ;     /* will be the output of this routine */
   int is;                        /* input stimulus index */


   if( ncall > 0 ) return NULL ;  /* generate interface for ncall 0 */


   /***** do interface #0 *****/

   /*---------------- set titles and call point ----------------*/

   plint = PLUTO_new_interface ("Deconvolution" ,
	   "Control DC_Fit, DC_Err, and DC_IRF Deconvolution Functions" ,
	   helpstring, PLUGIN_CALL_VIA_MENU, DC_main);

   global_plint = plint ;  /* make global copy */

   PLUTO_add_hint (plint, 
     "Control DC_Fit, DC_Err, and DC_IRF Deconvolution Functions");

   /*----- Parameters -----*/
   PLUTO_add_option (plint, "Control", "Control", TRUE);
   PLUTO_add_string (plint, "Baseline", NBASE, baseline_strings, 1);
   PLUTO_add_number (plint, "NFirst", 0, 1000, 0, 0,    TRUE);
   PLUTO_add_number (plint, "NLast",  0, 1000, 0, 1000, TRUE);
   PLUTO_add_string( plint, "IRF Label",    0, NULL, 1);


   /*----- Input Stimulus -----*/
   for (is = 0;  is < MAX_STIMTS;  is++)
     {
       PLUTO_add_option (plint, "Stimulus", "Stimulus", FALSE);
       PLUTO_add_string( plint, "Label", 0, NULL, 1);
       PLUTO_add_timeseries (plint, "File");
       PLUTO_add_number (plint, "Min Lag", 0, 100, 0, 0, TRUE);
       PLUTO_add_number (plint, "Max Lag", 0, 100, 0, 0, TRUE);
      
     }

   /*--------- done with interface setup ---------*/
   PLUTO_register_1D_funcstr ("DC_Fit" , DC_Fit);
   PLUTO_register_1D_funcstr ("DC_Err" , DC_Err);
   PLUTO_register_1D_funcstr ("DC_IRF" , DC_IRF);


  /*----- initialize matrices and vectors -----*/
  matrix_initialize (&x);
  matrix_initialize (&xtxinv);
  matrix_initialize (&xtxinvxt);
  matrix_initialize (&abase);
  matrix_initialize (&afull);

  for (is = 0;  is < MAX_STIMTS;  is++)
    {
      matrix_initialize (&ardcd[is]);
      stim_label[is] = malloc (sizeof(char)*MAX_NAME_LENGTH);
      MTEST (stim_label[is]);
    }

   return plint ;
}


/*---------------------------------------------------------------------------*/
/*
  Main routine for this plugin (will be called from AFNI).
  If the return string is not NULL, some error transpired, and
  AFNI will popup the return string in a message box.
*/

char * DC_main( PLUGIN_interface * plint )
{
  char * str;                           /* input string */
  int is;                               /* stimulus index */
  char IRF_label[MAX_NAME_LENGTH];      /* label of stimulus for IRF plot */
  

  /*--------- go to Control input line ---------*/
  PLUTO_next_option (plint);
  str    = PLUTO_get_string (plint);
  plug_polort = PLUTO_string_index (str, NBASE, baseline_strings);
  plug_NFirst = PLUTO_get_number (plint);
  plug_NLast  = PLUTO_get_number (plint);
  strcpy (IRF_label, PLUTO_get_string (plint));


  /*------ read Input Stimulus line(s) -----*/
  plug_IRF = -1;
  num_stimts = 0;
  do
    {
      str = PLUTO_get_optiontag(plint); 
      if (str == NULL)  break;
      if (strcmp (str, "Stimulus") != 0)
        return "************************\n"
               "Illegal optiontag found!\n"
               "************************";
     
      
      strcpy (stim_label[num_stimts], PLUTO_get_string(plint));
      if (strcmp(stim_label[num_stimts], IRF_label) == 0)
	plug_IRF = num_stimts;

      stimulus[num_stimts] = PLUTO_get_timeseries(plint) ;
  
      if (stimulus[num_stimts] == NULL || stimulus[num_stimts]->nx < 3 
	  ||  stimulus[num_stimts]->kind != MRI_float)
	return "*************************\n"
	       "Illegal Timeseries Input!\n"
	       "*************************"  ;

      min_lag[num_stimts] = PLUTO_get_number(plint);
      max_lag[num_stimts] = PLUTO_get_number(plint);

      if (min_lag[num_stimts] > max_lag[num_stimts])
	return "**************************\n"
	       "Require Min Lag <= Max Lag\n"
	       "**************************"  ;

      num_stimts++;

    }

  while (1);


  /*----- show current input options -----*/
  printf ("\n\n");
  printf ("Program: %s \n", PROGRAM_NAME);
  printf ("Author:  %s \n", PROGRAM_AUTHOR);
  printf ("Date:    %s \n", PROGRAM_DATE);
  printf ("\nControls: \n");
  printf ("Baseline  = %10s \n", baseline_strings[plug_polort]);
  printf ("NFirst    = %10d \n", plug_NFirst);
  printf ("NLast     = %10d \n", plug_NLast);
  printf ("IRF label = %10s \n", IRF_label);

  for (is = 0;  is < num_stimts;  is++)
    {
      printf ("\nStimulus:  %s \n", stim_label[is]);
      printf ("Min. Lag =%3d   Max. Lag =%3d \n", min_lag[is], max_lag[is]);
    }
  

  /*--- nothing left to do until data arrives ---*/
  initialize = 1 ;  /* force re-initialization */
  
  return NULL ;
}


/*---------------------------------------------------------------------------*/
/*
  Calculate the impulse response function and associated statistics.
*/

void calculate_results 
(
  int nt,               /* number of time points */
  double dt,            /* delta time */
  float * vec,          /* input measured data vector */
  int * nfirst,         /* first image from input 3d+time dataset to use */
  int * nlast,          /* last image from input 3d+time dataset to use */
  int * nfit,           /* number of fit parameters */
  float * fit,          /* fit parameters (regression coefficients) */
  char ** label         /* string containing statistical summary of results */
)
  
{
  float * ts_array;           /* array of measured data for one voxel */

  int N;                      /* number of data points */
  int p;                      /* number of parameters in the full model */
  int q;                      /* number of parameters in the baseline model */
  int m;                      /* parameter index */
  int n;                      /* data point index */

  vector coef;                /* regression parameters */
  vector scoef;               /* std. devs. for regression parameters */
  vector tcoef;               /* t-statistics for regression parameters */
  float fpart[MAX_STIMTS];    /* partial F-statistics for the stimuli */
  float freg;                 /* regression F-statistic */
  float rsqr;                 /* coeff. of multiple determination R^2  */

  vector y;                   /* vector of measured data */       

  int polort;            /* degree of polynomial for baseline model */
  int NFirst;            /* first image from input 3d+time dataset to use */
  int NLast;             /* last image from input 3d+time dataset to use */
  int i;                 /* data point index */
  int is;                /* stimulus index */

  float rms_min = 0.0;   /* minimum variation in data to fit full model */
  int ok;                /* flag for successful matrix calculation */


  /*----- Initialize vectors -----*/
  vector_initialize (&coef);
  vector_initialize (&scoef);
  vector_initialize (&tcoef);
  vector_initialize (&y);
  

  /*----- Initialize local variables -----*/
  polort = plug_polort;
  NFirst = plug_NFirst;
  NLast = plug_NLast;

  q = polort + 1;
  p = q;
  for (is = 0;  is < num_stimts;  is++)
    p += (max_lag[is]-min_lag[is]+1);

  for (is = 0;  is < num_stimts;  is++)
    if (NFirst < max_lag[is])  NFirst = max_lag[is];
   
  if (NLast > nt-1)  NLast = nt-1;
  N = NLast - NFirst + 1;

  *nfirst = NFirst;
  *nlast = NLast;
  *nfit = p;


  /*----- Initialize the independent variable matrix -----*/
  init_indep_var_matrix (p, q, NFirst, N, num_stimts,
			 stimulus, min_lag, max_lag, &x);


  /*----- Initialization for the regression analysis -----*/
  ok = init_regression_analysis (p, q, num_stimts, min_lag, max_lag,
				 x, &xtxinv, &xtxinvxt, &abase, ardcd, &afull);

  if (ok)
    {
      /*----- Extract Y-data for this voxel -----*/
      vector_create (N, &y);
      ts_array = vec;
      for (i = 0;  i < N;  i++)
	y.elts[i] = ts_array[i+NFirst];
      
      
      /*----- Perform the regression analysis -----*/
      regression_analysis (N, p, q, num_stimts, min_lag, max_lag,
			   xtxinv, xtxinvxt, abase, ardcd, afull, 
			   y, rms_min, &coef, &scoef, &tcoef, fpart, 
			   &freg, &rsqr);
      
      
      /*----- Save the fit parameters -----*/
      vector_to_array (coef, fit);
  
      
      /*----- Report results for this voxel -----*/
      printf ("\nResults for Voxel: \n");
      report_results (q, num_stimts, stim_label, min_lag, max_lag,
		      coef, tcoef, fpart, freg, rsqr, label);
      printf ("%s \n", *label);
    }

  else
    {
      vector_create (p, &coef);
      vector_to_array (coef, fit);
      strcpy (lbuf, "");
      *label = lbuf;
    }

  
  /*----- Dispose of vectors -----*/
  vector_destroy (&y);
  vector_destroy (&tcoef);
  vector_destroy (&scoef);
  vector_destroy (&coef);

}


/*---------------------------------------------------------------------------*/
/*
  Calculate the multiple linear regression least squares fit.
*/

void DC_Fit (int nt, double to, double dt, float * vec, char ** label)

{
  int NFirst;              /* first image from input 3d+time dataset to use */
  int NLast;               /* last image from input 3d+time dataset to use */
  float val;               /* fitted value at a time point */ 
  int n;                   /* time index */
  int ifit;                /* parameter index */
  int nfit;                /* number of fit parameters */
  float fit[MAX_XVARS];    /* fit parameters (regression coefficients) */


  /*----- Calculate the multiple linear regression -----*/
  calculate_results (nt, dt, vec, &NFirst, &NLast, &nfit, fit, label);


  /*----- Use the regression coefficients to calculate the fitted data -----*/
  for (n = NFirst;  n <= NLast;  n++)
    {
      val = 0.0;
      for (ifit = 0;  ifit < nfit;  ifit++)  
	val += x.elts[n-NFirst][ifit] * fit[ifit];
      vec[n] = val;
    }

  for (n = 0;  n < NFirst;  n++)
    vec[n] = vec[NFirst];

  for (n = NLast+1;  n < nt;  n++)
    vec[n] = vec[NLast];
  
  
  return;
}


/*---------------------------------------------------------------------------*/
/*
  Calculate the multiple linear regression residuals.
*/

void DC_Err (int nt, double to, double dt, float * vec, char ** label)

{
  int NFirst;              /* first image from input 3d+time dataset to use */
  int NLast;               /* last image from input 3d+time dataset to use */
  float val;               /* residual value at a time point */ 
  int n;                   /* time index */
  int ifit;                /* parameter index */
  int nfit;                /* number of fit parameters */
  float fit[MAX_XVARS];    /* fit parameters (regression coefficients) */


  /*----- Calculate the multiple linear regression -----*/
  calculate_results (nt, dt, vec, &NFirst, &NLast, &nfit, fit, label);


  /*----- Use the regression coefficients to calculate the residuals -----*/
  for (n = NFirst;  n <= NLast;  n++)
    {
      val = 0.0;
      for (ifit = 0;  ifit < nfit;  ifit++)  
	val += x.elts[n-NFirst][ifit] * fit[ifit];
      vec[n] = vec[n] - val;
    }

  for (n = 0;  n < NFirst;  n++)
    vec[n] = 0.0;

  for (n = NLast+1;  n < nt;  n++)
    vec[n] = 0.0;
  
  
  return;
}


/*---------------------------------------------------------------------------*/
/*
  Estimate the system impulse response function.
*/
 
void DC_IRF (int nt, double to, double dt, float * vec, char ** label)

{
  int NFirst;              /* first image from input 3d+time dataset to use */
  int NLast;               /* last image from input 3d+time dataset to use */
  int nfit;                /* number of fit parameters */
  float fit[MAX_XVARS];    /* fit parameters (regression coefficients) */
  int np;                  /* length of estimated impulse reponse function */
  int ip;                  /* impulse response function parameter index */
  int q;                   /* number of parameters in the baseline model */
  int it;                  /* array index */
  int ntdnp;               /* number of array points per IRF parameter */  


  /*----- Calculate the multiple linear regression -----*/
  calculate_results (nt, dt, vec, &NFirst, &NLast, &nfit, fit, label);


  /*----- If IRF index is invalid, return all zeros -----*/
  if (num_stimts == 1)
    plug_IRF = 0;
  else
    if ((plug_IRF < 0) || (plug_IRF >= num_stimts))
      {
	for (it = 0;  it < nt;  it++)
	  vec[it] = 0.0;
	return;
      }


  /*----- Plot the system impulse response function -----*/
  np = max_lag[plug_IRF] - min_lag[plug_IRF] + 1;
  ntdnp = nt / np;

  q = plug_polort+1;
  for (ip = 0;  ip < plug_IRF;  ip++)
    q += max_lag[ip] - min_lag[ip] + 1;

  for (it = 0;  it < np*ntdnp;  it++)
    {
      ip = q + it/ntdnp;
      vec[it] = fit[ip];
    }

  for (it = np*ntdnp;  it < nt;  it++)
    vec[it] = 0.0;

  
  return;
}


/*---------------------------------------------------------------------------*/







