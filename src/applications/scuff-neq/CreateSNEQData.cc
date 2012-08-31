/* Copyright (C) 2005-2011 M. T. Homer Reid
 *
 * This file is part of SCUFF-EM.
 *
 * SCUFF-EM is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * SCUFF-EM is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/*
 * CreateSNEQData.cc -- a utility function to initialize a 
 *                   -- scuff-neq data structure for a given
 *                   -- run of the code
 *
 * homer reid      -- 2/2012
 *
 */

#include "scuff-neq.h"

#define MAXSTR 1000

/***************************************************************/
/***************************************************************/
/***************************************************************/
SNEQData *CreateSNEQData(char *GeoFile, char *TransFile,
                         int QuantityFlags, int PlotFlux)
{

  SNEQData *SNEQD=(SNEQData *)mallocEC(sizeof(*SNEQD));

  SNEQD->WriteCache=0;
  SNEQD->PlotFlux=PlotFlux;

  /*--------------------------------------------------------------*/
  /*-- try to create the RWGGeometry -----------------------------*/
  /*--------------------------------------------------------------*/
  RWGGeometry *G=new RWGGeometry(GeoFile);
  G->SetLogLevel(SCUFF_VERBOSELOGGING);
  SNEQD->G=G;

  /*--------------------------------------------------------------*/
  /*- this code does not make sense if any of the objects are PEC */
  /*--------------------------------------------------------------*/
  int ns, nsp;
  for(ns=0; ns<G->NumSurfaces; ns++)
   if ( G->Surfaces[ns]->IsPEC ) 
    ErrExit("%s: object %s: PEC objects are not allowed in scuff-neq", G->GeoFileName,G->Surfaces[ns]->Label);

  /*--------------------------------------------------------------*/
  /*- read the transformation file if one was specified and check */
  /*- that it plays well with the specified geometry file.        */
  /*- note if TransFile==0 then this code snippet still works; in */
  /*- this case the list of GTComplices is initialized to contain */
  /*- a single empty GTComplex and the check automatically passes.*/
  /*--------------------------------------------------------------*/
  SNEQD->GTCList=ReadTransFile(TransFile, &(SNEQD->NumTransformations));
  char *ErrMsg=G->CheckGTCList(SNEQD->GTCList, SNEQD->NumTransformations);
  if (ErrMsg)
   ErrExit("file %s: %s",TransFile,ErrMsg);

  /*--------------------------------------------------------------*/
  /*- figure out which quantities were specified                 -*/
  /*--------------------------------------------------------------*/
  SNEQD->QuantityFlags=QuantityFlags;
  SNEQD->NQ=0;
  if ( QuantityFlags & QFLAG_POWER  ) SNEQD->NQ++;
  if ( QuantityFlags & QFLAG_XFORCE ) SNEQD->NQ++;
  if ( QuantityFlags & QFLAG_YFORCE ) SNEQD->NQ++;
  if ( QuantityFlags & QFLAG_ZFORCE ) SNEQD->NQ++;

  SNEQD->NSNQ = G->NumSurfaces * SNEQD->NQ; 
  SNEQD->NTNSNQ = SNEQD->NumTransformations * SNEQD->NSNQ;

  /*--------------------------------------------------------------*/
  /*- allocate arrays of matrix subblocks that allow us to reuse -*/
  /*- chunks of the BEM matrices for multiple geometrical        -*/
  /*- transformations.                                           -*/
  /*--------------------------------------------------------------*/
  int nb, nq, NS=G->NumSurfaces, NBF, NBFp;
  SNEQD->T = (HMatrix **)mallocEC(NS*sizeof(HMatrix *));
  SNEQD->U = (HMatrix **)mallocEC( ((NS*(NS-1))/2)*sizeof(HMatrix *));
  for(nb=ns=0; ns<G->NumSurfaces; ns++)
   { NBF=G->Surfaces[ns]->NumBFs;
     SNEQD->T[ns] = new HMatrix(NBF, NBF, LHM_COMPLEX, LHM_SYMMETRIC);
     for(nsp=ns+1; nsp<G->NumSurfaces; nsp++, nb++)
      { NBFp=G->Surfaces[nsp]->NumBFs;
        SNEQD->U[nb] = new HMatrix(NBF, NBFp, LHM_COMPLEX);
      };
   };

  /*--------------------------------------------------------------*/
  /*- allocate BEM matrix ----------------------------------------*/
  /*--------------------------------------------------------------*/
  SNEQD->W = new HMatrix(G->TotalBFs, G->TotalBFs, LHM_COMPLEX );

  /*--------------------------------------------------------------*/
  /*- allocate sparse matrices to store the various overlap      -*/
  /*- matrices. note that all overlap matrices have 10 nonzero   -*/
  /*- entries per row.                                           -*/
  /*-                                                            -*/
  /*- SArray[ns] is an array of SCUFF_NUM_OMATRICES pointers to  -*/
  /*- SMatrix structures for object #ns. SArray[ns][nom] is only -*/
  /*- non-NULL if we need the nomth type of overlap matrix. (Here-*/ 
  /*- nom=1,2,3,4 for power, x-force, y-force, z-force, as defined*/ 
  /*- in libscuff.h).                                             */ 
  /*-                                                            -*/
  /*--------------------------------------------------------------*/
  bool *NeedMatrix=SNEQD->NeedMatrix;
  memset(NeedMatrix, 0, SCUFF_NUM_OMATRICES*sizeof(int));
  NeedMatrix[SCUFF_OMATRIX_OVERLAP] = 0;
  NeedMatrix[SCUFF_OMATRIX_POWER  ] = QuantityFlags && QFLAG_POWER;
  NeedMatrix[SCUFF_OMATRIX_XFORCE ] = QuantityFlags && QFLAG_XFORCE;
  NeedMatrix[SCUFF_OMATRIX_YFORCE ] = QuantityFlags && QFLAG_YFORCE;
  NeedMatrix[SCUFF_OMATRIX_ZFORCE ] = QuantityFlags && QFLAG_ZFORCE;

  SNEQD->SArray=(SMatrix ***)mallocEC(NS*sizeof(SMatrix **));
  for(ns=0; ns<NS; ns++)
   { 
     SNEQD->SArray[ns]=(SMatrix **)mallocEC(SCUFF_NUM_OMATRICES*sizeof(SMatrix *));

     for(int nom=0; nom<SCUFF_NUM_OMATRICES; nom++)
      if (NeedMatrix[nom]) 
       SNEQD->SArray[ns][nom] = new SMatrix(G->Surfaces[ns]->NumBFs,G->Surfaces[ns]->NumBFs,LHM_COMPLEX);
   };

  /*--------------------------------------------------------------*/
  /*- create frequency-resolved output files for each object in  -*/
  /*- the geometry and write a file header to each file.         -*/
  /*--------------------------------------------------------------*/
  SNEQD->ByOmegaFileNames=0;
  int WriteByOmegaFiles=1;
  if (WriteByOmegaFiles)
   { 
     SNEQD->ByOmegaFileNames=(char **)mallocEC(NS*NS*sizeof(char *));
     FILE *f;
     for(ns=0; ns<NS; ns++)
      for(nsp=0; nsp<NS; nsp++)
       { 
         SNEQD->ByOmegaFileNames[ns*NS+nsp] = vstrdup("From%sTo%s.byOmega",
                                                       G->Surfaces[ns]->Label,
                                                       G->Surfaces[nsp]->Label);
   
         f=fopen(SNEQD->ByOmegaFileNames[ns*NS+nsp],"a");
         if (!f)
          ErrExit("could not create file %s",SNEQD->ByOmegaFileNames[ns]);
         fprintf(f,"# data file columns: \n");
         fprintf(f,"# 1: angular frequency in units of 3e14 rad/sec \n");
         fprintf(f,"# 2: transformation tag \n");
        
         nq=3;
         if (QuantityFlags && QFLAG_POWER)
          fprintf(f,"# %i: spectral density of power flux \n",nq++);
         if (QuantityFlags && QFLAG_XFORCE)
          fprintf(f,"# %i: spectral density of x-momentum flux\n",nq++);
         if (QuantityFlags && QFLAG_YFORCE)
          fprintf(f,"# %i: spectral density of y-momentum flux\n",nq++);
         if (QuantityFlags && QFLAG_ZFORCE)
          fprintf(f,"# %i: spectral density of z-momentum flux\n",nq++);
   
         fclose(f);
       }; // for ( ns = ...) for (nsp = ...)

   }; //if (WriteByOmegaFiles)

  return SNEQD;

}