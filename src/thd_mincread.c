#include "mrilib.h"
#include "netcdf.h"

/*******************************************************************/
/******* 29 Oct 2001: Read a 3D MINC file as an AFNI dataset *******/
/*******************************************************************/

static int first_err = 1 ;
static char *fname_err ;

#define EPR(x,s1,s2)                                                       \
 do{ if( first_err ){ fprintf(stderr,"\n"); first_err=0; }                 \
     fprintf(stderr,"** NetCDF error [%s,%s]: %s\n",s1,s2,nc_strerror(x)); \
 } while(0)

/*-----------------------------------------------------------------
  Read the attributes relevant to a MINC dimension variable
-------------------------------------------------------------------*/

typedef struct {
   int dimid , varid , good , afni_orient ;
   size_t len ;
   float start , step , xcos,ycos,zcos ;
   char spacetype[32] ;
} mincdim ;

static mincdim read_mincdim( int ncid , char *dname )
{
   mincdim ddd ;
   int code ;
   float ccc[3] ;
   size_t lll ;
   static char *ename=NULL ;

   ddd.good = 0 ;  /* flag for bad result */

   lll = strlen(fname_err) + strlen(dname) + 4 ;
   ename = realloc( ename , lll) ;
   sprintf(ename,"%s:%s",fname_err,dname) ;

   /* get ID of this dimension name */

   code = nc_inq_dimid( ncid , dname , &ddd.dimid ) ;
   if( code != NC_NOERR ){ EPR(code,ename,"dimid"); return ddd; }

   /* get length of this dimension */

   code = nc_inq_dimlen( ncid , ddd.dimid , &ddd.len ) ;
   if( code != NC_NOERR ){ EPR(code,ename,"dimlen"); return ddd; }

   /* get ID of corresponding variable */

   code = nc_inq_varid( ncid , dname , &ddd.varid ) ;
   if( code != NC_NOERR ){ EPR(code,ename,"varid"); return ddd; }

   /* get step attribute of this variable */

   code = nc_get_att_float( ncid , ddd.varid , "step" , &ddd.step ) ;
   if( code != NC_NOERR ){ EPR(code,ename,"step"); return ddd; }
   if( ddd.step == 0.0 ){
      if( first_err ){ fprintf(stderr,"\n"); first_err=0; }
      fprintf(stderr,"** MINC error: %s:step=0\n",ename);
      return ddd ;
   }

   /* get start attribute of this variable */

   code = nc_get_att_float( ncid , ddd.varid , "start" , &ddd.start ) ;
   if( code != NC_NOERR ){
      if( first_err ){ fprintf(stderr,"\n"); first_err=0; }
      fprintf(stderr,"** MINC warning: can't read %s:start attribute\n",ename) ;
      ddd.start = -0.5*ddd.step*ddd.len ;
   }

   /* get direction_cosines attribute of this variable [not used] */

   code = nc_get_att_float( ncid , ddd.varid , "direction_cosines" , ccc ) ;
   if( code == NC_NOERR ){
      ddd.xcos = ccc[0] ; ddd.ycos = ccc[1] ; ddd.zcos = ccc[2] ;
   } else {
      ddd.xcos = ddd.ycos = ddd.zcos = 0.0 ;
   }

   /* get spacetype attribute of this variable [Talairach or not?] */

   ddd.spacetype[0] = '\0' ; lll = 0 ;
   code = nc_inq_attlen( ncid , ddd.varid , "spacetype" , &lll ) ;
   if( code == NC_NOERR && lll > 0 && lll < 32 ){
      code = nc_get_att_text( ncid,ddd.varid , "spacetype" , ddd.spacetype ) ;
      if( code == NC_NOERR ){
         ddd.spacetype[lll] = '\0' ;
      } else {
         ddd.spacetype[0] = '\0' ;
      }
   }

   ddd.good = 1 ; return ddd ;
}

/*-----------------------------------------------------------------
  Open a MINC file as an AFNI dataset
-------------------------------------------------------------------*/

THD_3dim_dataset * THD_open_minc( char *pathname )
{
   THD_3dim_dataset *dset=NULL ;
   int ncid , code ;
   mincdim xspace , yspace , zspace , *xyz[3] ;
   int im_varid , im_rank , im_dimid[4] ;
   nc_type im_type ;

   char prefix[1024] , *ppp , tname[12] ;
   THD_ivec3 nxyz , orixyz ;
   THD_fvec3 dxyz , orgxyz ;
   int datum , nvals=1 , ibr , iview ;
   size_t len ;
   float dtime=1.0 ;

ENTRY("THD_open_minc") ;

   /*-- open input file --*/

   if( pathname == NULL || pathname[0] == '\0' ) RETURN(NULL);

   first_err = 1 ;        /* put a newline before 1st error message */
   fname_err = pathname ; /* filename for errors in read_mincdim()  */

   code = nc_open( pathname , NC_NOWRITE , &ncid ) ;
   if( code != NC_NOERR ){ EPR(code,pathname,"open"); RETURN(NULL); }

   /*---------- get info about spatial axes ---------*/
   /*[[ MINC x and y are reversed relative to AFNI ]]*/

   xspace = read_mincdim( ncid , "xspace" ) ;
   xspace.step = -xspace.step ; xspace.start = -xspace.start ;
   xspace.afni_orient = (xspace.step < 0.0) ? ORI_L2R_TYPE : ORI_R2L_TYPE ;

   yspace = read_mincdim( ncid , "yspace" ) ;
   yspace.step = -yspace.step ; yspace.start = -yspace.start ;
   yspace.afni_orient = (yspace.step < 0.0) ? ORI_P2A_TYPE : ORI_A2P_TYPE ;

   zspace = read_mincdim( ncid , "zspace" ) ;
   zspace.afni_orient = (zspace.step > 0.0) ? ORI_I2S_TYPE : ORI_S2I_TYPE ;

   if( !xspace.good || !yspace.good || !zspace.good ){
      nc_close(ncid) ; RETURN(NULL) ;
   }

   /*-- get info about the image data --*/

   code = nc_inq_varid( ncid , "image" , &im_varid ) ;
   if( code != NC_NOERR ){ EPR(code,pathname,"image varid"); nc_close(ncid); RETURN(NULL); }

   if( !AFNI_yesenv("AFNI_MINC_FLOATIZE") ){  /* determine type of dataset values */
      code = nc_inq_vartype( ncid , im_varid , &im_type ) ;
      if( code != NC_NOERR ){ EPR(code,pathname,"image vartype"); nc_close(ncid); RETURN(NULL); }
      switch( im_type ){
         default:
            if( first_err ){ fprintf(stderr,"\n"); first_err=0; }
            fprintf(stderr,"** MINC error: can't handle image type code %d\n",im_type);
            nc_close(ncid); RETURN(NULL);
         break ;

         case NC_BYTE:  datum = MRI_byte  ; break ;

         case NC_SHORT: datum = MRI_short ; break ;

         case NC_FLOAT:
         case NC_DOUBLE:
         case NC_INT:   datum = MRI_float ; break ;
      }
   } else {                               /* always read in as floats if */
      datum = MRI_float ;                 /* AFNI_MINC_FLOATIZE is Yes   */
   }

   /* we allow nD mages only for n=3 or n=4 */

   code = nc_inq_varndims( ncid , im_varid , &im_rank ) ;
   if( code != NC_NOERR ){ EPR(code,pathname,"image varndims"); nc_close(ncid); RETURN(NULL); }
   if( im_rank < 3 || im_rank > 4 ){
      if( first_err ){ fprintf(stderr,"\n"); first_err=0; }
      fprintf(stderr,"** MINC error: image rank=%d\n",im_rank) ;
      nc_close(ncid); RETURN(NULL);
   }

   code = nc_inq_vardimid( ncid , im_varid , im_dimid ) ;
   if( code != NC_NOERR ){ EPR(code,pathname,"image vardimid"); nc_close(ncid); RETURN(NULL); }

   /*-- find axes order --*/

     /* fastest variation */

        if( im_dimid[im_rank-1] == xspace.dimid ) xyz[0] = &xspace ;
   else if( im_dimid[im_rank-1] == yspace.dimid ) xyz[0] = &yspace ;
   else if( im_dimid[im_rank-1] == zspace.dimid ) xyz[0] = &zspace ;
   else {
      if( first_err ){ fprintf(stderr,"\n"); first_err=0; }
      fprintf(stderr,"** MINC error: image dim[2] illegal\n") ;
      nc_close(ncid) ; RETURN(NULL) ;
   }

     /* next fastest variation */

        if( im_dimid[im_rank-2] == xspace.dimid ) xyz[1] = &xspace ;
   else if( im_dimid[im_rank-2] == yspace.dimid ) xyz[1] = &yspace ;
   else if( im_dimid[im_rank-2] == zspace.dimid ) xyz[1] = &zspace ;
   else {
      if( first_err ){ fprintf(stderr,"\n"); first_err=0; }
      fprintf(stderr,"** MINC error: image dim[1] illegal\n") ;
      nc_close(ncid) ; RETURN(NULL) ;
   }

     /* slowest variation */

        if( im_dimid[im_rank-3] == xspace.dimid ) xyz[2] = &xspace ;
   else if( im_dimid[im_rank-3] == yspace.dimid ) xyz[2] = &yspace ;
   else if( im_dimid[im_rank-3] == zspace.dimid ) xyz[2] = &zspace ;
   else {
      if( first_err ){ fprintf(stderr,"\n"); first_err=0; }
      fprintf(stderr,"** MINC error: image dim[0] illegal\n") ;
      nc_close(ncid) ; RETURN(NULL) ;
   }

   /*-- determine the length of the 4th dimension, if any --*/

   strcpy(tname,"MINC") ;  /* default name for 4th dimension */

   if( im_rank == 4 ){
      char fname[NC_MAX_NAME+4] ;
      code = nc_inq_dimlen( ncid , im_dimid[0] , &len ) ;
      if( code != NC_NOERR ){ EPR(code,pathname,"dimid[0] dimlen"); nc_close(ncid); RETURN(NULL); }

      nvals = len ;  /* number of volumes in this file */

      /* get the name of this dimension */

      code = nc_inq_dimname( ncid , im_dimid[0] , fname ) ;
      if( code == NC_NOERR ){
         MCW_strncpy(tname,fname,10) ;
      }

      /* get the stepsize of this dimension */

      code = nc_get_att_float( ncid , im_dimid[0] , "step" , &dtime ) ;
      if( code != NC_NOERR || dtime <= 0.0 ) dtime = 1.0 ;
   }

   /*-- make a dataset --*/

   dset = EDIT_empty_copy(NULL) ;

   ppp  = THD_trailname(pathname,0) ;               /* strip directory */
   MCW_strncpy( prefix , ppp , THD_MAX_PREFIX ) ;   /* to make prefix */

   nxyz.ijk[0] = xyz[0]->len ; dxyz.xyz[0] = xyz[0]->step ;  /* setup axes */
   nxyz.ijk[1] = xyz[1]->len ; dxyz.xyz[1] = xyz[1]->step ;
   nxyz.ijk[2] = xyz[2]->len ; dxyz.xyz[2] = xyz[2]->step ;

   orixyz.ijk[0] = xyz[0]->afni_orient ; orgxyz.xyz[0] = xyz[0]->start ;
   orixyz.ijk[1] = xyz[1]->afni_orient ; orgxyz.xyz[1] = xyz[1]->start ;
   orixyz.ijk[2] = xyz[2]->afni_orient ; orgxyz.xyz[2] = xyz[2]->start ;

   if( strstr(xyz[0]->spacetype,"alairach") != NULL ){  /* coord system */
      iview = VIEW_TALAIRACH_TYPE ;
   } else {
      iview = VIEW_ORIGINAL_TYPE ;
   }

   dset->idcode.str[0] = 'M' ;  /* this is a fake ID code */
   dset->idcode.str[1] = 'I' ;
   dset->idcode.str[2] = 'N' ;
   dset->idcode.str[3] = 'C' ;
   dset->idcode.str[4] = ':' ;

   EDIT_dset_items( dset ,
                      ADN_prefix      , prefix ,
                      ADN_datum_all   , datum ,
                      ADN_nxyz        , nxyz ,
                      ADN_xyzdel      , dxyz ,
                      ADN_xyzorg      , orgxyz ,
                      ADN_xyzorient   , orixyz ,
                      ADN_malloc_type , DATABLOCK_MEM_MALLOC ,
                      ADN_nvals       , nvals ,
                      ADN_type        , HEAD_ANAT_TYPE ,
                      ADN_view_type   , iview ,
                      ADN_func_type   , (nvals==1) ? ANAT_MRAN_TYPE : ANAT_BUCK_TYPE ,
                    ADN_none ) ;

   if( nvals > 9 )              /* pretend it is 3D+time */
      EDIT_dset_items( dset ,
                         ADN_func_type, ANAT_EPI_TYPE ,
                         ADN_ntt      , nvals ,
                         ADN_ttorg    , 0.0 ,
                         ADN_ttdel    , dtime ,
                         ADN_ttdur    , 0.0 ,
                         ADN_tunits   , UNITS_SEC_TYPE ,
                       ADN_none ) ;

   dset->dblk->diskptr->storage_mode = STORAGE_BY_MINC ;
   strcpy( dset->dblk->diskptr->brick_name , pathname ) ;

   for( ibr=0 ; ibr < nvals ; ibr++ ){     /* make sub-brick labels */
      sprintf(prefix,"%s[%d]",tname,ibr) ;
      EDIT_BRICK_LABEL( dset , ibr , prefix ) ;
   }

   /*-- read the global:history attribute --*/

   sprintf(prefix,"THD_open_minc(%s)",pathname) ;
   tross_Append_History( dset , prefix ) ;

   code = nc_inq_attlen( ncid , NC_GLOBAL , "history" , &len ) ;
   if( code == NC_NOERR && len > 0 ){
      ppp = malloc(len+4) ;
      code = nc_get_att_text( ncid , NC_GLOBAL , "history" , ppp ) ;
      if( code == NC_NOERR ){  /* should always happen */
         ppp[len] = '\0' ;
         tross_Append_History( dset , ppp ) ;
      }
      free(ppp) ;
   }

   nc_close(ncid) ; RETURN(dset) ;
}

/*-----------------------------------------------------------------
  Load a MINC dataset's data into memory
-------------------------------------------------------------------*/

void THD_load_minc( THD_datablock *dblk )
{
   THD_diskptr *dkptr ;
   int im_varid , code , ncid ;
   int nx,ny,nz,nxy,nxyz,nxyzv , nbad,ibr,nv, nslice ;
   void *ptr ;
   nc_type im_type=NC_SHORT ;
   float im_valid_range[2], fac,denom , intop,inbot , outbot,outtop ;
   float *im_min=NULL  , *im_max=NULL  ;
   int    im_min_varid ,  im_max_varid ;

ENTRY("THD_load_minc") ;

   /*-- open input [these errors should never occur] --*/

   if( !ISVALID_DATABLOCK(dblk)                       ||
       dblk->diskptr->storage_mode != STORAGE_BY_MINC ||
       dblk->brick == NULL                              ) EXRETURN ;

   dkptr = dblk->diskptr ;

   code = nc_open( dkptr->brick_name , NC_NOWRITE , &ncid ) ;
   if( code != NC_NOERR ){ EPR(code,dkptr->brick_name,"open"); EXRETURN; }

   code = nc_inq_varid( ncid , "image" , &im_varid ) ;
   if( code != NC_NOERR ){ EPR(code,dkptr->brick_name,"image varid"); nc_close(ncid); EXRETURN; }

   /*-- allocate data space --*/

   nx = dkptr->dimsizes[0] ;
   ny = dkptr->dimsizes[1] ;  nxy   = nx * ny   ;
   nz = dkptr->dimsizes[2] ;  nxyz  = nxy * nz  ;
   nv = dkptr->nvals       ;  nxyzv = nxyz * nv ; nslice = nz*nv ;

   dblk->malloc_type = DATABLOCK_MEM_MALLOC ;

   /** malloc space for each brick separately **/

   for( nbad=ibr=0 ; ibr < nv ; ibr++ ){
      if( DBLK_ARRAY(dblk,ibr) == NULL ){
         ptr = malloc( DBLK_BRICK_BYTES(dblk,ibr) ) ;
         mri_fix_data_pointer( ptr ,  DBLK_BRICK(dblk,ibr) ) ;
         if( ptr == NULL ) nbad++ ;
      }
   }

   /** if couldn't get them all, take our ball and go home in a sulk **/

   if( nbad > 0 ){
      fprintf(stderr,"\n** failed to malloc %d MINC bricks out of %d\n\a",nbad,nv) ;
      for( ibr=0 ; ibr < nv ; ibr++ ){
        if( DBLK_ARRAY(dblk,ibr) != NULL ){
           free(DBLK_ARRAY(dblk,ibr)) ;
           mri_fix_data_pointer( NULL , DBLK_BRICK(dblk,ibr) ) ;
        }
      }
      nc_close(ncid) ; EXRETURN ;
   }

   /** get scaling attributes/variables **/

   (void) nc_inq_vartype( ncid , im_varid , &im_type ) ;

   code = nc_get_att_float( ncid,im_varid , "valid_range" , im_valid_range ) ;
   if( code != NC_NOERR || im_valid_range[0] >= im_valid_range[1] ){

      im_valid_range[0] = 0.0 ;

      switch( im_type ){
         case NC_BYTE:   im_valid_range[1] = 255.0        ; break ;
         case NC_SHORT:  im_valid_range[1] = 32767.0      ; break ;
         case NC_INT:    im_valid_range[1] = 2147483647.0 ; break ;
         case NC_FLOAT:
         case NC_DOUBLE: im_valid_range[1] = 1.0          ; break ;
      break ;
      }
   }
   inbot = im_valid_range[0] ;
   intop = im_valid_range[1] ;
   denom = intop - inbot  ;  /* always positive */

   code = nc_inq_varid( ncid , "image-min" , &im_min_varid ) ;
   if( code == NC_NOERR ){
      im_min = (float *) calloc(sizeof(float),nslice) ;
      code = nc_get_var_float( ncid , im_min_varid , im_min ) ;
   }

   if( im_min != NULL ){
      code = nc_inq_varid( ncid , "image-max" , &im_max_varid ) ;
      if( code == NC_NOERR ){
         im_max = (float *) malloc(sizeof(float)*nslice) ;
         code = nc_get_var_float( ncid , im_max_varid , im_max ) ;
         if( code != NC_NOERR ){
            free(im_min) ; im_min = NULL ;
            free(im_max) ; im_max = NULL ;
         }
      }
   }

   /** read data! **/

   switch( DBLK_BRICK_TYPE(dblk,0) ){  /* output datum */

      /* this case should never happen */

      default:
         fprintf(stderr,"\n** MINC illegal datum %d found\n",
                 DBLK_BRICK_TYPE(dblk,0)                     ) ;
         for( ibr=0 ; ibr < nv ; ibr++ ){
           if( DBLK_ARRAY(dblk,ibr) != NULL ){
              free(DBLK_ARRAY(dblk,ibr)) ;
              mri_fix_data_pointer( NULL , DBLK_BRICK(dblk,ibr) ) ;
           }
         }
      break ;

      case MRI_byte:{
         int kk,ii,qq ; byte *br ;

         if( nv == 1 ){
            code = nc_get_var_uchar( ncid , im_varid , DBLK_ARRAY(dblk,0) ) ;
         } else {
            size_t start[4] , count[4] ;
            start[1] = start[2] = start[3] = 0 ;
            count[0] = 1 ; count[1] = nz ; count[2] = ny ; count[3] = nx ;
            for( ibr=0 ; ibr < nv ; ibr++ ){
               start[0] = ibr ;
               code = nc_get_vara_uchar( ncid,im_varid ,
                                         start,count , DBLK_ARRAY(dblk,ibr) ) ;
            }
         }
         if( code != NC_NOERR ) EPR(code,dkptr->brick_name,"image get_var_uchar") ;

         if( im_max != NULL ){                 /* must scale values */
           for( ibr=0 ; ibr < nv ; ibr++ ){     /* loop over bricks */
             br = DBLK_ARRAY(dblk,ibr) ;
             for( kk=0 ; kk < nz ; kk++ ){      /* loop over slices */
                outbot = im_min[kk+ibr*nz] ;
                outtop = im_max[kk+ibr*nz] ;
                if( outbot >= outtop) continue ;            /* skip */
                fac = (outtop-outbot) / denom ;
                qq = kk*nxy ; outbot += 0.499 ;
                for( ii=0 ; ii < nxy ; ii++ )        /* scale slice */
                   br[ii+qq] = (byte) (fac*br[ii+qq] + outbot) ;
             }
           }
         }
      }
      break ;

      case MRI_short:{
         int kk,ii,qq ; short *br ;

         if( nv == 1 ){
            code = nc_get_var_short( ncid , im_varid , DBLK_ARRAY(dblk,0) ) ;
         } else {
            size_t start[4] , count[4] ;
            start[1] = start[2] = start[3] = 0 ;
            count[0] = 1 ; count[1] = nz ; count[2] = ny ; count[3] = nx ;
            for( ibr=0 ; ibr < nv ; ibr++ ){
               start[0] = ibr ;
               code = nc_get_vara_short( ncid,im_varid ,
                                         start,count , DBLK_ARRAY(dblk,ibr) ) ;
            }
         }
         if( code != NC_NOERR ) EPR(code,dkptr->brick_name,"image get_var_short") ;

         if( im_max != NULL ){                 /* must scale values */
           for( ibr=0 ; ibr < nv ; ibr++ ){     /* loop over bricks */
             br = DBLK_ARRAY(dblk,ibr) ;
             for( kk=0 ; kk < nz ; kk++ ){      /* loop over slices */
                outbot = im_min[kk+ibr*nz] ;
                outtop = im_max[kk+ibr*nz] ;
                if( outbot >= outtop) continue ;            /* skip */
                fac = (outtop-outbot) / denom ;
                qq = kk*nxy ; outbot += 0.499 ;
                for( ii=0 ; ii < nxy ; ii++ )        /* scale slice */
                   br[ii+qq] = (short) (fac*br[ii+qq] + outbot) ;
             }
           }
         }
      }
      break ;

      case MRI_float:{
         int kk,ii,qq ; float *br ;

         if( nv == 1 ){
            code = nc_get_var_float( ncid , im_varid , DBLK_ARRAY(dblk,0) ) ;
         } else {
            size_t start[4] , count[4] ;
            start[1] = start[2] = start[3] = 0 ;
            count[0] = 1 ; count[1] = nz ; count[2] = ny ; count[3] = nx ;
            for( ibr=0 ; ibr < nv ; ibr++ ){
               start[0] = ibr ;
               code = nc_get_vara_float( ncid,im_varid ,
                                         start,count , DBLK_ARRAY(dblk,ibr) ) ;
            }
         }
         if( code != NC_NOERR ) EPR(code,dkptr->brick_name,"image get_var_float") ;

         if( im_type == NC_BYTE ){              /* fix sign of bytes */
           for( ibr=0 ; ibr < nv ; ibr++ ){     /* loop over bricks */
             br = DBLK_ARRAY(dblk,ibr) ;
             for( ii=0 ; ii < nxyz ; ii++ )
                if( br[ii] < 0.0 ) br[ii] += 256.0 ;
           }
         }

         if( im_max != NULL ){                 /* must scale values */
           for( ibr=0 ; ibr < nv ; ibr++ ){     /* loop over bricks */
             br = DBLK_ARRAY(dblk,ibr) ;
             for( kk=0 ; kk < nz ; kk++ ){      /* loop over slices */
                outbot = im_min[kk+ibr*nz] ;
                outtop = im_max[kk+ibr*nz] ;
                if( outbot >= outtop) continue ;            /* skip */
                fac = (outtop-outbot) / denom ;
                qq = kk*nxy ;
                for( ii=0 ; ii < nxy ; ii++ )        /* scale slice */
                   br[ii+qq] = (fac*br[ii+qq] + outbot) ;
             }
           }
         }
      }
      break ;
   }

   if( im_min != NULL ) free(im_min) ;
   if( im_max != NULL ) free(im_max) ;
   nc_close(ncid) ; EXRETURN ;
}
