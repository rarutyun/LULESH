/*
  This is a Version 2.0 MPI + OpenMP implementation of LULESH

                 Copyright (c) 2010-2013.
      Lawrence Livermore National Security, LLC.
Produced at the Lawrence Livermore National Laboratory.
                  LLNL-CODE-461231
                All rights reserved.

This file is part of LULESH, Version 2.0.
Please also read this link -- http://www.opensource.org/licenses/index.php

//////////////
DIFFERENCES BETWEEN THIS VERSION (2.x) AND EARLIER VERSIONS:
* Addition of regions to make work more representative of multi-material codes
* Default size of each domain is 30^3 (27000 elem) instead of 45^3. This is
  more representative of our actual working set sizes
* Single source distribution supports pure serial, pure OpenMP, MPI-only,
  and MPI+OpenMP
* Addition of ability to visualize the mesh using VisIt
  https://wci.llnl.gov/codes/visit/download.html
* Various command line options (see ./lulesh2.0 -h)
 -q              : quiet mode - suppress stdout
 -i <iterations> : number of cycles to run
 -s <size>       : length of cube mesh along side
 -r <numregions> : Number of distinct regions (def: 11)
 -b <balance>    : Load balance between regions of a domain (def: 1)
 -c <cost>       : Extra cost of more expensive regions (def: 1)
 -f <filepieces> : Number of file parts for viz output (def: np/9)
 -p              : Print out progress
 -v              : Output viz file (requires compiling with -DVIZ_MESH
 -h              : This message

 // printf("Usage: %s [opts]\n", execname);
      // printf(" where [opts] is one or more of:\n");
      // printf(" -q              : quiet mode - suppress all stdout\n");
      // printf(" -i <iterations> : number of cycles to run\n");
      // printf(" -s <size>       : length of cube mesh along side\n");
      // printf(" -r <numregions> : Number of distinct regions (def: 11)\n");
      // printf(" -b <balance>    : Load balance between regions of a domain (def:
1)\n"); // printf(" -c <cost>       : Extra cost of more expensive regions (def:
1)\n"); // printf(" -f <numfiles>   : Number of files to split viz dump into (def:
(np+10)/9)\n"); // printf(" -p              : Print out progress\n"); // printf(" -v
: Output viz file (requires compiling with -DVIZ_MESH\n"); // printf(" -h : This
message\n"); // printf("\n\n");

*Notable changes in LULESH 2.0

* Split functionality into different files
lulesh.cc - where most (all?) of the timed functionality lies
lulesh-comm.cc - MPI functionality
lulesh-init.cc - Setup code
lulesh-viz.cc  - Support for visualization option
lulesh-util.cc - Non-timed functions
*
* The concept of "regions" was added, although every region is the same ideal
*    gas material, and the same sedov blast wave problem is still the only
*    problem its hardcoded to solve.
* Regions allow two things important to making this proxy app more
representative:
*   Four of the LULESH routines are now performed on a region-by-region basis,
*     making the memory access patterns non-unit stride
*   Artificial load imbalances can be easily introduced that could impact
*     parallelization strategies.
* The load balance flag changes region assignment.  Region number is raised to
*   the power entered for assignment probability.  Most likely regions changes
*   with MPI process id.
* The cost flag raises the cost of ~45% of the regions to evaluate EOS by the
*   entered multiple. The cost of 5% is 10x the entered multiple.
* MPI and OpenMP were added, and coalesced into a single version of the source
*   that can support serial builds, MPI-only, OpenMP-only, and MPI+OpenMP
* Added support to write plot files using "poor mans parallel I/O" when linked
*   with the silo library, which in turn can be read by VisIt.
* Enabled variable timestep calculation by default (courant condition), which
*   results in an additional reduction.
* Default domain (mesh) size reduced from 45^3 to 30^3
* Command line options to allow numerous test cases without needing to recompile
* Performance optimizations and code cleanup beyond LULESH 1.0
* Added a "Figure of Merit" calculation (elements solved per microsecond) and
*   output in support of using LULESH 2.0 for the 2017 CORAL procurement
*
* Possible Differences in Final Release (other changes possible)
*
* High Level mesh structure to allow data structure transformations
* Different default parameters
* Minor code performance changes and cleanup

TODO in future versions
* Add reader for (truly) unstructured meshes, probably serial only
* CMake based build system

//////////////

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

   * Redistributions of source code must retain the above copyright
     notice, this list of conditions and the disclaimer below.

   * Redistributions in binary form must reproduce the above copyright
     notice, this list of conditions and the disclaimer (as noted below)
     in the documentation and/or other materials provided with the
     distribution.

   * Neither the name of the LLNS/LLNL nor the names of its contributors
     may be used to endorse or promote products derived from this software
     without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL LAWRENCE LIVERMORE NATIONAL SECURITY, LLC,
THE U.S. DEPARTMENT OF ENERGY OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.


Additional BSD Notice

1. This notice is required to be provided under our contract with the U.S.
   Department of Energy (DOE). This work was produced at Lawrence Livermore
   National Laboratory under Contract No. DE-AC52-07NA27344 with the DOE.

2. Neither the United States Government nor Lawrence Livermore National
   Security, LLC nor any of their employees, makes any warranty, express
   or implied, or assumes any liability or responsibility for the accuracy,
   completeness, or usefulness of any information, apparatus, product, or
   process disclosed, or represents that its use would not infringe
   privately-owned rights.

3. Also, reference herein to any specific commercial products, process, or
   services by trade name, trademark, manufacturer or otherwise does not
   necessarily constitute or imply its endorsement, recommendation, or
   favoring by the United States Government or Lawrence Livermore National
   Security, LLC. The views and opinions of authors expressed herein do not
   necessarily state or reflect those of the United States Government or
   Lawrence Livermore National Security, LLC, and shall not be used for
   advertising or product endorsement purposes.

*/

#include <climits>
#include <ctype.h>
#include <iostream>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <vector>

#include <algorithm>
#include <execution>
#include <numeric>

#if _OPENMP
#include <omp.h>
#endif

#include "lulesh.h"

#ifdef USE_CUDA
#include <cuda.h>
#include <cuda_runtime.h>
#include <cuda_runtime_api.h>
#include <cuda_profiler_api.h>
#endif

/* Work Routines */

static inline void TimeIncrement(Domain &domain) {
  Real_t targetdt = domain.stoptime() - domain.time();

  if ((domain.dtfixed() <= Real_t(0.0)) && (domain.cycle() != Int_t(0))) {
    Real_t ratio;
    Real_t olddt = domain.deltatime();

    /* This will require a reduction in parallel */
    Real_t gnewdt = Real_t(1.0e+20);
    Real_t newdt;
    if (domain.dtcourant() < gnewdt) {
      gnewdt = domain.dtcourant() / Real_t(2.0);
    }
    if (domain.dthydro() < gnewdt) {
      gnewdt = domain.dthydro() * Real_t(2.0) / Real_t(3.0);
    }

    newdt = gnewdt;

    ratio = newdt / olddt;
    if (ratio >= Real_t(1.0)) {
      if (ratio < domain.deltatimemultlb()) {
        newdt = olddt;
      } else if (ratio > domain.deltatimemultub()) {
        newdt = olddt * domain.deltatimemultub();
      }
    }

    if (newdt > domain.dtmax()) {
      newdt = domain.dtmax();
    }
    domain.deltatime() = newdt;
  }

  /* TRY TO PREVENT VERY SMALL SCALING ON THE NEXT CYCLE */
  if ((targetdt > domain.deltatime()) &&
      (targetdt < (Real_t(4.0) * domain.deltatime() / Real_t(3.0)))) {
    targetdt = Real_t(2.0) * domain.deltatime() / Real_t(3.0);
  }

  if (targetdt < domain.deltatime()) {
    domain.deltatime() = targetdt;
  }

  domain.time() += domain.deltatime();

  ++domain.cycle();
}

/******************************************/

static inline void CollectDomainNodesToElemNodes(Domain &domain,
                                                 const Index_t *elemToNode,
                                                 Real_t elemX[8],
                                                 Real_t elemY[8],
                                                 Real_t elemZ[8]) {
  Index_t nd0i = elemToNode[0];
  Index_t nd1i = elemToNode[1];
  Index_t nd2i = elemToNode[2];
  Index_t nd3i = elemToNode[3];
  Index_t nd4i = elemToNode[4];
  Index_t nd5i = elemToNode[5];
  Index_t nd6i = elemToNode[6];
  Index_t nd7i = elemToNode[7];

  elemX[0] = domain.x(nd0i);
  elemX[1] = domain.x(nd1i);
  elemX[2] = domain.x(nd2i);
  elemX[3] = domain.x(nd3i);
  elemX[4] = domain.x(nd4i);
  elemX[5] = domain.x(nd5i);
  elemX[6] = domain.x(nd6i);
  elemX[7] = domain.x(nd7i);

  elemY[0] = domain.y(nd0i);
  elemY[1] = domain.y(nd1i);
  elemY[2] = domain.y(nd2i);
  elemY[3] = domain.y(nd3i);
  elemY[4] = domain.y(nd4i);
  elemY[5] = domain.y(nd5i);
  elemY[6] = domain.y(nd6i);
  elemY[7] = domain.y(nd7i);

  elemZ[0] = domain.z(nd0i);
  elemZ[1] = domain.z(nd1i);
  elemZ[2] = domain.z(nd2i);
  elemZ[3] = domain.z(nd3i);
  elemZ[4] = domain.z(nd4i);
  elemZ[5] = domain.z(nd5i);
  elemZ[6] = domain.z(nd6i);
  elemZ[7] = domain.z(nd7i);
}

#ifdef LULESH_MEASURE_EACH
template <typename S, typename F>
auto ms(const S& s, const F& f) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(f - s).count();
}
#endif

/******************************************/

static inline void InitStressTermsForElems(Domain &domain, Real_t *sigxx,
                                           Real_t *sigyy, Real_t *sigzz,
                                           Index_t numElem) {
  //
  // pull in the stresses appropriate to the hydro integration
  //

  Domain* domain_ptr = &domain;
#ifdef LULESH_MEASURE_EACH
  auto start = std::chrono::high_resolution_clock::now();
#endif
  std::for_each_n(std::execution::par_unseq, counting_iterator(0), numElem,
                  [=](Index_t i) {
                    sigxx[i] = sigyy[i] = sigzz[i] = -domain_ptr->p(i) - domain_ptr->q(i);
                  });
#ifdef LULESH_MEASURE_EACH
  auto finish = std::chrono::high_resolution_clock::now();
  std::cout << "std::for_each_n on the line 283 elapsed time: " << ms(start, finish) << "ms problem size " << numElem << std::endl;
#endif
}

/******************************************/

static inline void CalcElemShapeFunctionDerivatives(Real_t const x[],
                                                    Real_t const y[],
                                                    Real_t const z[],
                                                    Real_t b[][8],
                                                    Real_t *const volume) {
  const Real_t x0 = x[0];
  const Real_t x1 = x[1];
  const Real_t x2 = x[2];
  const Real_t x3 = x[3];
  const Real_t x4 = x[4];
  const Real_t x5 = x[5];
  const Real_t x6 = x[6];
  const Real_t x7 = x[7];

  const Real_t y0 = y[0];
  const Real_t y1 = y[1];
  const Real_t y2 = y[2];
  const Real_t y3 = y[3];
  const Real_t y4 = y[4];
  const Real_t y5 = y[5];
  const Real_t y6 = y[6];
  const Real_t y7 = y[7];

  const Real_t z0 = z[0];
  const Real_t z1 = z[1];
  const Real_t z2 = z[2];
  const Real_t z3 = z[3];
  const Real_t z4 = z[4];
  const Real_t z5 = z[5];
  const Real_t z6 = z[6];
  const Real_t z7 = z[7];

  Real_t fjxxi, fjxet, fjxze;
  Real_t fjyxi, fjyet, fjyze;
  Real_t fjzxi, fjzet, fjzze;
  Real_t cjxxi, cjxet, cjxze;
  Real_t cjyxi, cjyet, cjyze;
  Real_t cjzxi, cjzet, cjzze;

  fjxxi = Real_t(.125) * ((x6 - x0) + (x5 - x3) - (x7 - x1) - (x4 - x2));
  fjxet = Real_t(.125) * ((x6 - x0) - (x5 - x3) + (x7 - x1) - (x4 - x2));
  fjxze = Real_t(.125) * ((x6 - x0) + (x5 - x3) + (x7 - x1) + (x4 - x2));

  fjyxi = Real_t(.125) * ((y6 - y0) + (y5 - y3) - (y7 - y1) - (y4 - y2));
  fjyet = Real_t(.125) * ((y6 - y0) - (y5 - y3) + (y7 - y1) - (y4 - y2));
  fjyze = Real_t(.125) * ((y6 - y0) + (y5 - y3) + (y7 - y1) + (y4 - y2));

  fjzxi = Real_t(.125) * ((z6 - z0) + (z5 - z3) - (z7 - z1) - (z4 - z2));
  fjzet = Real_t(.125) * ((z6 - z0) - (z5 - z3) + (z7 - z1) - (z4 - z2));
  fjzze = Real_t(.125) * ((z6 - z0) + (z5 - z3) + (z7 - z1) + (z4 - z2));

  /* compute cofactors */
  cjxxi = (fjyet * fjzze) - (fjzet * fjyze);
  cjxet = -(fjyxi * fjzze) + (fjzxi * fjyze);
  cjxze = (fjyxi * fjzet) - (fjzxi * fjyet);

  cjyxi = -(fjxet * fjzze) + (fjzet * fjxze);
  cjyet = (fjxxi * fjzze) - (fjzxi * fjxze);
  cjyze = -(fjxxi * fjzet) + (fjzxi * fjxet);

  cjzxi = (fjxet * fjyze) - (fjyet * fjxze);
  cjzet = -(fjxxi * fjyze) + (fjyxi * fjxze);
  cjzze = (fjxxi * fjyet) - (fjyxi * fjxet);

  /* calculate partials :
     this need only be done for l = 0,1,2,3   since , by symmetry ,
     (6,7,4,5) = - (0,1,2,3) .
  */
  b[0][0] = -cjxxi - cjxet - cjxze;
  b[0][1] = cjxxi - cjxet - cjxze;
  b[0][2] = cjxxi + cjxet - cjxze;
  b[0][3] = -cjxxi + cjxet - cjxze;
  b[0][4] = -b[0][2];
  b[0][5] = -b[0][3];
  b[0][6] = -b[0][0];
  b[0][7] = -b[0][1];

  b[1][0] = -cjyxi - cjyet - cjyze;
  b[1][1] = cjyxi - cjyet - cjyze;
  b[1][2] = cjyxi + cjyet - cjyze;
  b[1][3] = -cjyxi + cjyet - cjyze;
  b[1][4] = -b[1][2];
  b[1][5] = -b[1][3];
  b[1][6] = -b[1][0];
  b[1][7] = -b[1][1];

  b[2][0] = -cjzxi - cjzet - cjzze;
  b[2][1] = cjzxi - cjzet - cjzze;
  b[2][2] = cjzxi + cjzet - cjzze;
  b[2][3] = -cjzxi + cjzet - cjzze;
  b[2][4] = -b[2][2];
  b[2][5] = -b[2][3];
  b[2][6] = -b[2][0];
  b[2][7] = -b[2][1];

  /* calculate jacobian determinant (volume) */
  *volume = Real_t(8.) * (fjxet * cjxet + fjyet * cjyet + fjzet * cjzet);
}

/******************************************/

static inline void SumElemFaceNormal(
    Real_t *normalX0, Real_t *normalY0, Real_t *normalZ0, Real_t *normalX1,
    Real_t *normalY1, Real_t *normalZ1, Real_t *normalX2, Real_t *normalY2,
    Real_t *normalZ2, Real_t *normalX3, Real_t *normalY3, Real_t *normalZ3,
    const Real_t x0, const Real_t y0, const Real_t z0, const Real_t x1,
    const Real_t y1, const Real_t z1, const Real_t x2, const Real_t y2,
    const Real_t z2, const Real_t x3, const Real_t y3, const Real_t z3) {
  Real_t bisectX0 = Real_t(0.5) * (x3 + x2 - x1 - x0);
  Real_t bisectY0 = Real_t(0.5) * (y3 + y2 - y1 - y0);
  Real_t bisectZ0 = Real_t(0.5) * (z3 + z2 - z1 - z0);
  Real_t bisectX1 = Real_t(0.5) * (x2 + x1 - x3 - x0);
  Real_t bisectY1 = Real_t(0.5) * (y2 + y1 - y3 - y0);
  Real_t bisectZ1 = Real_t(0.5) * (z2 + z1 - z3 - z0);
  Real_t areaX = Real_t(0.25) * (bisectY0 * bisectZ1 - bisectZ0 * bisectY1);
  Real_t areaY = Real_t(0.25) * (bisectZ0 * bisectX1 - bisectX0 * bisectZ1);
  Real_t areaZ = Real_t(0.25) * (bisectX0 * bisectY1 - bisectY0 * bisectX1);

  *normalX0 += areaX;
  *normalX1 += areaX;
  *normalX2 += areaX;
  *normalX3 += areaX;

  *normalY0 += areaY;
  *normalY1 += areaY;
  *normalY2 += areaY;
  *normalY3 += areaY;

  *normalZ0 += areaZ;
  *normalZ1 += areaZ;
  *normalZ2 += areaZ;
  *normalZ3 += areaZ;
}

/******************************************/

static inline void CalcElemNodeNormals(Real_t pfx[8], Real_t pfy[8],
                                       Real_t pfz[8], const Real_t x[8],
                                       const Real_t y[8], const Real_t z[8]) {
  for (Index_t i = 0; i < 8; ++i) {
    pfx[i] = Real_t(0.0);
    pfy[i] = Real_t(0.0);
    pfz[i] = Real_t(0.0);
  }
  /* evaluate face one: nodes 0, 1, 2, 3 */
  SumElemFaceNormal(&pfx[0], &pfy[0], &pfz[0], &pfx[1], &pfy[1], &pfz[1],
                    &pfx[2], &pfy[2], &pfz[2], &pfx[3], &pfy[3], &pfz[3], x[0],
                    y[0], z[0], x[1], y[1], z[1], x[2], y[2], z[2], x[3], y[3],
                    z[3]);
  /* evaluate face two: nodes 0, 4, 5, 1 */
  SumElemFaceNormal(&pfx[0], &pfy[0], &pfz[0], &pfx[4], &pfy[4], &pfz[4],
                    &pfx[5], &pfy[5], &pfz[5], &pfx[1], &pfy[1], &pfz[1], x[0],
                    y[0], z[0], x[4], y[4], z[4], x[5], y[5], z[5], x[1], y[1],
                    z[1]);
  /* evaluate face three: nodes 1, 5, 6, 2 */
  SumElemFaceNormal(&pfx[1], &pfy[1], &pfz[1], &pfx[5], &pfy[5], &pfz[5],
                    &pfx[6], &pfy[6], &pfz[6], &pfx[2], &pfy[2], &pfz[2], x[1],
                    y[1], z[1], x[5], y[5], z[5], x[6], y[6], z[6], x[2], y[2],
                    z[2]);
  /* evaluate face four: nodes 2, 6, 7, 3 */
  SumElemFaceNormal(&pfx[2], &pfy[2], &pfz[2], &pfx[6], &pfy[6], &pfz[6],
                    &pfx[7], &pfy[7], &pfz[7], &pfx[3], &pfy[3], &pfz[3], x[2],
                    y[2], z[2], x[6], y[6], z[6], x[7], y[7], z[7], x[3], y[3],
                    z[3]);
  /* evaluate face five: nodes 3, 7, 4, 0 */
  SumElemFaceNormal(&pfx[3], &pfy[3], &pfz[3], &pfx[7], &pfy[7], &pfz[7],
                    &pfx[4], &pfy[4], &pfz[4], &pfx[0], &pfy[0], &pfz[0], x[3],
                    y[3], z[3], x[7], y[7], z[7], x[4], y[4], z[4], x[0], y[0],
                    z[0]);
  /* evaluate face six: nodes 4, 7, 6, 5 */
  SumElemFaceNormal(&pfx[4], &pfy[4], &pfz[4], &pfx[7], &pfy[7], &pfz[7],
                    &pfx[6], &pfy[6], &pfz[6], &pfx[5], &pfy[5], &pfz[5], x[4],
                    y[4], z[4], x[7], y[7], z[7], x[6], y[6], z[6], x[5], y[5],
                    z[5]);
}

/******************************************/

static inline void
SumElemStressesToNodeForces(const Real_t B[][8], const Real_t stress_xx,
                            const Real_t stress_yy, const Real_t stress_zz,
                            Real_t fx[], Real_t fy[], Real_t fz[]) {
  for (Index_t i = 0; i < 8; i++) {
    fx[i] = -(stress_xx * B[0][i]);
    fy[i] = -(stress_yy * B[1][i]);
    fz[i] = -(stress_zz * B[2][i]);
  }
}

/******************************************/

static inline void IntegrateStressForElems(Domain &domain, Real_t *sigxx,
                                           Real_t *sigyy, Real_t *sigzz,
                                           Real_t *determ, Index_t numElem,
                                           Index_t numNode) {
  Index_t numElem8 = numElem * 8;
  Real_t *fx_elem;
  Real_t *fy_elem;
  Real_t *fz_elem;

  domain.fx_elem.resize(numElem8);
  domain.fy_elem.resize(numElem8);
  domain.fz_elem.resize(numElem8);
  fx_elem = domain.fx_elem.data();
  fy_elem = domain.fy_elem.data();
  fz_elem = domain.fz_elem.data();


  // loop over all elements

#ifdef LULESH_MEASURE_EACH
  auto start = std::chrono::high_resolution_clock::now();
#endif

  Domain* domain_ptr = &domain;
  std::for_each_n(
      std::execution::par_unseq, counting_iterator(0), numElem,
      [=](Index_t k) {
        const Index_t *const elemToNode = domain_ptr->nodelist(k);
        Real_t B[3][8]; // shape function derivatives
        Real_t x_local[8];
        Real_t y_local[8];
        Real_t z_local[8];

        // get nodal coordinates from global arrays and copy into local arrays.
        CollectDomainNodesToElemNodes(*domain_ptr, elemToNode, x_local, y_local,
                                      z_local);

        // Volume calculation involves extra work for numerical consistency
        CalcElemShapeFunctionDerivatives(x_local, y_local, z_local, B,
                                         &determ[k]);

        CalcElemNodeNormals(B[0], B[1], B[2], x_local, y_local, z_local);

        // Eliminate thread writing conflicts at the nodes by giving
        // each element its own copy to write to
        SumElemStressesToNodeForces(B, sigxx[k], sigyy[k], sigzz[k],
                                    &fx_elem[k * 8], &fy_elem[k * 8],
                                    &fz_elem[k * 8]);
      });

#ifdef LULESH_MEASURE_EACH
  auto finish = std::chrono::high_resolution_clock::now();
  std::cout << "std::for_each_n on the line 517 elapsed time: " << ms(start, finish) << "ms problem size " << numElem << std::endl;

  start = std::chrono::high_resolution_clock::now();
#endif

  // If threaded, then we need to copy the data out of the temporary
  // arrays used above into the final forces field
  std::for_each_n(std::execution::par_unseq, counting_iterator(0), numNode,
                  [=](Index_t gnode) {
                    Index_t count = domain_ptr->nodeElemCount(gnode);
                    Index_t *cornerList = domain_ptr->nodeElemCornerList(gnode);
                    Real_t fx_tmp = Real_t(0.0);
                    Real_t fy_tmp = Real_t(0.0);
                    Real_t fz_tmp = Real_t(0.0);
                    for (Index_t i = 0; i < count; ++i) {
                      Index_t ielem = cornerList[i];
                      fx_tmp += fx_elem[ielem];
                      fy_tmp += fy_elem[ielem];
                      fz_tmp += fz_elem[ielem];
                    }
                    domain_ptr->fx(gnode) = fx_tmp;
                    domain_ptr->fy(gnode) = fy_tmp;
                    domain_ptr->fz(gnode) = fz_tmp;
                  });

#ifdef LULESH_MEASURE_EACH
  finish = std::chrono::high_resolution_clock::now();
  std::cout << "std::for_each_n on the line 552 elapsed time: " << ms(start, finish) << "ms problem size " << numElem << std::endl;
#endif
}

/******************************************/

static inline void VoluDer(const Real_t x0, const Real_t x1, const Real_t x2,
                           const Real_t x3, const Real_t x4, const Real_t x5,
                           const Real_t y0, const Real_t y1, const Real_t y2,
                           const Real_t y3, const Real_t y4, const Real_t y5,
                           const Real_t z0, const Real_t z1, const Real_t z2,
                           const Real_t z3, const Real_t z4, const Real_t z5,
                           Real_t *dvdx, Real_t *dvdy, Real_t *dvdz) {
  const Real_t twelfth = Real_t(1.0) / Real_t(12.0);

  *dvdx = (y1 + y2) * (z0 + z1) - (y0 + y1) * (z1 + z2) +
          (y0 + y4) * (z3 + z4) - (y3 + y4) * (z0 + z4) -
          (y2 + y5) * (z3 + z5) + (y3 + y5) * (z2 + z5);
  *dvdy = -(x1 + x2) * (z0 + z1) + (x0 + x1) * (z1 + z2) -
          (x0 + x4) * (z3 + z4) + (x3 + x4) * (z0 + z4) +
          (x2 + x5) * (z3 + z5) - (x3 + x5) * (z2 + z5);

  *dvdz = -(y1 + y2) * (x0 + x1) + (y0 + y1) * (x1 + x2) -
          (y0 + y4) * (x3 + x4) + (y3 + y4) * (x0 + x4) +
          (y2 + y5) * (x3 + x5) - (y3 + y5) * (x2 + x5);

  *dvdx *= twelfth;
  *dvdy *= twelfth;
  *dvdz *= twelfth;
}

/******************************************/

static inline void CalcElemVolumeDerivative(Real_t dvdx[8], Real_t dvdy[8],
                                            Real_t dvdz[8], const Real_t x[8],
                                            const Real_t y[8],
                                            const Real_t z[8]) {
  VoluDer(x[1], x[2], x[3], x[4], x[5], x[7], y[1], y[2], y[3], y[4], y[5],
          y[7], z[1], z[2], z[3], z[4], z[5], z[7], &dvdx[0], &dvdy[0],
          &dvdz[0]);
  VoluDer(x[0], x[1], x[2], x[7], x[4], x[6], y[0], y[1], y[2], y[7], y[4],
          y[6], z[0], z[1], z[2], z[7], z[4], z[6], &dvdx[3], &dvdy[3],
          &dvdz[3]);
  VoluDer(x[3], x[0], x[1], x[6], x[7], x[5], y[3], y[0], y[1], y[6], y[7],
          y[5], z[3], z[0], z[1], z[6], z[7], z[5], &dvdx[2], &dvdy[2],
          &dvdz[2]);
  VoluDer(x[2], x[3], x[0], x[5], x[6], x[4], y[2], y[3], y[0], y[5], y[6],
          y[4], z[2], z[3], z[0], z[5], z[6], z[4], &dvdx[1], &dvdy[1],
          &dvdz[1]);
  VoluDer(x[7], x[6], x[5], x[0], x[3], x[1], y[7], y[6], y[5], y[0], y[3],
          y[1], z[7], z[6], z[5], z[0], z[3], z[1], &dvdx[4], &dvdy[4],
          &dvdz[4]);
  VoluDer(x[4], x[7], x[6], x[1], x[0], x[2], y[4], y[7], y[6], y[1], y[0],
          y[2], z[4], z[7], z[6], z[1], z[0], z[2], &dvdx[5], &dvdy[5],
          &dvdz[5]);
  VoluDer(x[5], x[4], x[7], x[2], x[1], x[3], y[5], y[4], y[7], y[2], y[1],
          y[3], z[5], z[4], z[7], z[2], z[1], z[3], &dvdx[6], &dvdy[6],
          &dvdz[6]);
  VoluDer(x[6], x[5], x[4], x[3], x[2], x[0], y[6], y[5], y[4], y[3], y[2],
          y[0], z[6], z[5], z[4], z[3], z[2], z[0], &dvdx[7], &dvdy[7],
          &dvdz[7]);
}

/******************************************/

static inline void CalcElemFBHourglassForce(Real_t *xd, Real_t *yd, Real_t *zd,
                                            Real_t hourgam[][4],
                                            Real_t coefficient, Real_t *hgfx,
                                            Real_t *hgfy, Real_t *hgfz) {
  Real_t hxx[4];
  for (Index_t i = 0; i < 4; i++) {
    hxx[i] = hourgam[0][i] * xd[0] + hourgam[1][i] * xd[1] +
             hourgam[2][i] * xd[2] + hourgam[3][i] * xd[3] +
             hourgam[4][i] * xd[4] + hourgam[5][i] * xd[5] +
             hourgam[6][i] * xd[6] + hourgam[7][i] * xd[7];
  }
  for (Index_t i = 0; i < 8; i++) {
    hgfx[i] = coefficient * (hourgam[i][0] * hxx[0] + hourgam[i][1] * hxx[1] +
                             hourgam[i][2] * hxx[2] + hourgam[i][3] * hxx[3]);
  }
  for (Index_t i = 0; i < 4; i++) {
    hxx[i] = hourgam[0][i] * yd[0] + hourgam[1][i] * yd[1] +
             hourgam[2][i] * yd[2] + hourgam[3][i] * yd[3] +
             hourgam[4][i] * yd[4] + hourgam[5][i] * yd[5] +
             hourgam[6][i] * yd[6] + hourgam[7][i] * yd[7];
  }
  for (Index_t i = 0; i < 8; i++) {
    hgfy[i] = coefficient * (hourgam[i][0] * hxx[0] + hourgam[i][1] * hxx[1] +
                             hourgam[i][2] * hxx[2] + hourgam[i][3] * hxx[3]);
  }
  for (Index_t i = 0; i < 4; i++) {
    hxx[i] = hourgam[0][i] * zd[0] + hourgam[1][i] * zd[1] +
             hourgam[2][i] * zd[2] + hourgam[3][i] * zd[3] +
             hourgam[4][i] * zd[4] + hourgam[5][i] * zd[5] +
             hourgam[6][i] * zd[6] + hourgam[7][i] * zd[7];
  }
  for (Index_t i = 0; i < 8; i++) {
    hgfz[i] = coefficient * (hourgam[i][0] * hxx[0] + hourgam[i][1] * hxx[1] +
                             hourgam[i][2] * hxx[2] + hourgam[i][3] * hxx[3]);
  }
}

/******************************************/

static inline void CalcFBHourglassForceForElems(Domain &domain, Real_t *determ,
                                                Real_t *x8n, Real_t *y8n,
                                                Real_t *z8n, Real_t *dvdx,
                                                Real_t *dvdy, Real_t *dvdz,
                                                Real_t hourg, Index_t numElem,
                                                Index_t numNode) {
  /*************************************************
   *
   *     FUNCTION: Calculates the Flanagan-Belytschko anti-hourglass
   *               force.
   *
   *************************************************/

  Index_t numElem8 = numElem * 8;

  Real_t *fx_elem;
  Real_t *fy_elem;
  Real_t *fz_elem;

  domain.fx_elem.resize(numElem8);
  domain.fy_elem.resize(numElem8);
  domain.fz_elem.resize(numElem8);
  fx_elem = domain.fx_elem.data();
  fy_elem = domain.fy_elem.data();
  fz_elem = domain.fz_elem.data();

  Real_t gamma[4][8];

  gamma[0][0] = Real_t(1.);
  gamma[0][1] = Real_t(1.);
  gamma[0][2] = Real_t(-1.);
  gamma[0][3] = Real_t(-1.);
  gamma[0][4] = Real_t(-1.);
  gamma[0][5] = Real_t(-1.);
  gamma[0][6] = Real_t(1.);
  gamma[0][7] = Real_t(1.);
  gamma[1][0] = Real_t(1.);
  gamma[1][1] = Real_t(-1.);
  gamma[1][2] = Real_t(-1.);
  gamma[1][3] = Real_t(1.);
  gamma[1][4] = Real_t(-1.);
  gamma[1][5] = Real_t(1.);
  gamma[1][6] = Real_t(1.);
  gamma[1][7] = Real_t(-1.);
  gamma[2][0] = Real_t(1.);
  gamma[2][1] = Real_t(-1.);
  gamma[2][2] = Real_t(1.);
  gamma[2][3] = Real_t(-1.);
  gamma[2][4] = Real_t(1.);
  gamma[2][5] = Real_t(-1.);
  gamma[2][6] = Real_t(1.);
  gamma[2][7] = Real_t(-1.);
  gamma[3][0] = Real_t(-1.);
  gamma[3][1] = Real_t(1.);
  gamma[3][2] = Real_t(-1.);
  gamma[3][3] = Real_t(1.);
  gamma[3][4] = Real_t(1.);
  gamma[3][5] = Real_t(-1.);
  gamma[3][6] = Real_t(1.);
  gamma[3][7] = Real_t(-1.);

  /*************************************************/
  /*    compute the hourglass modes */

  Domain* domain_ptr = &domain;
  std::for_each_n(
      std::execution::par_unseq, counting_iterator(0), numElem,
      [=](Index_t i2) {
        Real_t *fx_local, *fy_local, *fz_local;
        Real_t hgfx[8], hgfy[8], hgfz[8];

        Real_t coefficient;

        Real_t hourgam[8][4];
        Real_t xd1[8], yd1[8], zd1[8];

        const Index_t *elemToNode = domain_ptr->nodelist(i2);
        Index_t i3 = 8 * i2;
        Real_t volinv = Real_t(1.0) / determ[i2];
        Real_t ss1, mass1, volume13;
        for (Index_t i1 = 0; i1 < 4; ++i1) {

          Real_t hourmodx =
              x8n[i3] * gamma[i1][0] + x8n[i3 + 1] * gamma[i1][1] +
              x8n[i3 + 2] * gamma[i1][2] + x8n[i3 + 3] * gamma[i1][3] +
              x8n[i3 + 4] * gamma[i1][4] + x8n[i3 + 5] * gamma[i1][5] +
              x8n[i3 + 6] * gamma[i1][6] + x8n[i3 + 7] * gamma[i1][7];

          Real_t hourmody =
              y8n[i3] * gamma[i1][0] + y8n[i3 + 1] * gamma[i1][1] +
              y8n[i3 + 2] * gamma[i1][2] + y8n[i3 + 3] * gamma[i1][3] +
              y8n[i3 + 4] * gamma[i1][4] + y8n[i3 + 5] * gamma[i1][5] +
              y8n[i3 + 6] * gamma[i1][6] + y8n[i3 + 7] * gamma[i1][7];

          Real_t hourmodz =
              z8n[i3] * gamma[i1][0] + z8n[i3 + 1] * gamma[i1][1] +
              z8n[i3 + 2] * gamma[i1][2] + z8n[i3 + 3] * gamma[i1][3] +
              z8n[i3 + 4] * gamma[i1][4] + z8n[i3 + 5] * gamma[i1][5] +
              z8n[i3 + 6] * gamma[i1][6] + z8n[i3 + 7] * gamma[i1][7];

          hourgam[0][i1] = gamma[i1][0] -
                           volinv * (dvdx[i3] * hourmodx + dvdy[i3] * hourmody +
                                     dvdz[i3] * hourmodz);

          hourgam[1][i1] = gamma[i1][1] - volinv * (dvdx[i3 + 1] * hourmodx +
                                                    dvdy[i3 + 1] * hourmody +
                                                    dvdz[i3 + 1] * hourmodz);

          hourgam[2][i1] = gamma[i1][2] - volinv * (dvdx[i3 + 2] * hourmodx +
                                                    dvdy[i3 + 2] * hourmody +
                                                    dvdz[i3 + 2] * hourmodz);

          hourgam[3][i1] = gamma[i1][3] - volinv * (dvdx[i3 + 3] * hourmodx +
                                                    dvdy[i3 + 3] * hourmody +
                                                    dvdz[i3 + 3] * hourmodz);

          hourgam[4][i1] = gamma[i1][4] - volinv * (dvdx[i3 + 4] * hourmodx +
                                                    dvdy[i3 + 4] * hourmody +
                                                    dvdz[i3 + 4] * hourmodz);

          hourgam[5][i1] = gamma[i1][5] - volinv * (dvdx[i3 + 5] * hourmodx +
                                                    dvdy[i3 + 5] * hourmody +
                                                    dvdz[i3 + 5] * hourmodz);

          hourgam[6][i1] = gamma[i1][6] - volinv * (dvdx[i3 + 6] * hourmodx +
                                                    dvdy[i3 + 6] * hourmody +
                                                    dvdz[i3 + 6] * hourmodz);

          hourgam[7][i1] = gamma[i1][7] - volinv * (dvdx[i3 + 7] * hourmodx +
                                                    dvdy[i3 + 7] * hourmody +
                                                    dvdz[i3 + 7] * hourmodz);
        }

        /* compute forces */
        /* store forces into h arrays (force arrays) */

        ss1 = domain_ptr->ss(i2);
        mass1 = domain_ptr->elemMass(i2);
        volume13 = CBRT(determ[i2]);

        Index_t n0si2 = elemToNode[0];
        Index_t n1si2 = elemToNode[1];
        Index_t n2si2 = elemToNode[2];
        Index_t n3si2 = elemToNode[3];
        Index_t n4si2 = elemToNode[4];
        Index_t n5si2 = elemToNode[5];
        Index_t n6si2 = elemToNode[6];
        Index_t n7si2 = elemToNode[7];

        xd1[0] = domain_ptr->xd(n0si2);
        xd1[1] = domain_ptr->xd(n1si2);
        xd1[2] = domain_ptr->xd(n2si2);
        xd1[3] = domain_ptr->xd(n3si2);
        xd1[4] = domain_ptr->xd(n4si2);
        xd1[5] = domain_ptr->xd(n5si2);
        xd1[6] = domain_ptr->xd(n6si2);
        xd1[7] = domain_ptr->xd(n7si2);

        yd1[0] = domain_ptr->yd(n0si2);
        yd1[1] = domain_ptr->yd(n1si2);
        yd1[2] = domain_ptr->yd(n2si2);
        yd1[3] = domain_ptr->yd(n3si2);
        yd1[4] = domain_ptr->yd(n4si2);
        yd1[5] = domain_ptr->yd(n5si2);
        yd1[6] = domain_ptr->yd(n6si2);
        yd1[7] = domain_ptr->yd(n7si2);

        zd1[0] = domain_ptr->zd(n0si2);
        zd1[1] = domain_ptr->zd(n1si2);
        zd1[2] = domain_ptr->zd(n2si2);
        zd1[3] = domain_ptr->zd(n3si2);
        zd1[4] = domain_ptr->zd(n4si2);
        zd1[5] = domain_ptr->zd(n5si2);
        zd1[6] = domain_ptr->zd(n6si2);
        zd1[7] = domain_ptr->zd(n7si2);

        coefficient = -hourg * Real_t(0.01) * ss1 * mass1 / volume13;

        CalcElemFBHourglassForce(xd1, yd1, zd1, hourgam, coefficient, hgfx,
                                 hgfy, hgfz);

        // With the threaded version, we write into local arrays per elem
        // so we don't have to worry about race conditions
        fx_local = &fx_elem[i3];
        fx_local[0] = hgfx[0];
        fx_local[1] = hgfx[1];
        fx_local[2] = hgfx[2];
        fx_local[3] = hgfx[3];
        fx_local[4] = hgfx[4];
        fx_local[5] = hgfx[5];
        fx_local[6] = hgfx[6];
        fx_local[7] = hgfx[7];

        fy_local = &fy_elem[i3];
        fy_local[0] = hgfy[0];
        fy_local[1] = hgfy[1];
        fy_local[2] = hgfy[2];
        fy_local[3] = hgfy[3];
        fy_local[4] = hgfy[4];
        fy_local[5] = hgfy[5];
        fy_local[6] = hgfy[6];
        fy_local[7] = hgfy[7];

        fz_local = &fz_elem[i3];
        fz_local[0] = hgfz[0];
        fz_local[1] = hgfz[1];
        fz_local[2] = hgfz[2];
        fz_local[3] = hgfz[3];
        fz_local[4] = hgfz[4];
        fz_local[5] = hgfz[5];
        fz_local[6] = hgfz[6];
        fz_local[7] = hgfz[7];
      });

  // Collect the data from the local arrays into the final force arrays
  std::for_each_n(std::execution::par_unseq, counting_iterator(0), numNode,
                  [=](Index_t gnode) {
                    Index_t count = domain_ptr->nodeElemCount(gnode);
                    Index_t *cornerList = domain_ptr->nodeElemCornerList(gnode);
                    Real_t fx_tmp = Real_t(0.0);
                    Real_t fy_tmp = Real_t(0.0);
                    Real_t fz_tmp = Real_t(0.0);
                    for (Index_t i = 0; i < count; ++i) {
                      Index_t ielem = cornerList[i];
                      fx_tmp += fx_elem[ielem];
                      fy_tmp += fy_elem[ielem];
                      fz_tmp += fz_elem[ielem];
                    }
                    domain_ptr->fx(gnode) += fx_tmp;
                    domain_ptr->fy(gnode) += fy_tmp;
                    domain_ptr->fz(gnode) += fz_tmp;
                  });
}

/******************************************/

#ifdef LULESH_ALLOCATE_EXP
struct FancyDeleter {
  template <typename T>
  void operator()(T* ptr) {
    Release(&ptr);
  }
};

using RealTPtr = std::unique_ptr<Real_t, FancyDeleter>;
using MultRealTPtrs = std::unique_ptr<RealTPtr[], FancyDeleter>;

RealTPtr dvdx_g;
RealTPtr dvdy_g;
RealTPtr dvdz_g;
RealTPtr x8n_g;
RealTPtr y8n_g;
RealTPtr z8n_g;
RealTPtr sigxx_g;
RealTPtr sigyy_g;
RealTPtr sigzz_g;
RealTPtr determ_g;
RealTPtr vnewc_g;

MultRealTPtrs e_olds_g;
MultRealTPtrs delvcs_g;
MultRealTPtrs p_olds_g;
MultRealTPtrs q_olds_g;
MultRealTPtrs compressions_g;
MultRealTPtrs compHalfSteps_g;
MultRealTPtrs qq_olds_g;
MultRealTPtrs ql_olds_g;
MultRealTPtrs works_g;
MultRealTPtrs p_news_g;
MultRealTPtrs e_news_g;
MultRealTPtrs q_news_g;
MultRealTPtrs bvcs_g;
MultRealTPtrs pbvcs_g;
MultRealTPtrs pHalfSteps_g;

void allocateGlobals(Domain* domain) {
  Index_t numElem = domain->numElem();
  Index_t numReg = domain->numReg();
  Index_t numElem8 = numElem * 8;

  dvdx_g.reset(Allocate<Real_t>(numElem8));
  dvdy_g.reset(Allocate<Real_t>(numElem8));
  dvdz_g.reset(Allocate<Real_t>(numElem8));

  x8n_g.reset(Allocate<Real_t>(numElem8));
  y8n_g.reset(Allocate<Real_t>(numElem8));
  z8n_g.reset(Allocate<Real_t>(numElem8));

  sigxx_g.reset(Allocate<Real_t>(numElem));
  sigyy_g.reset(Allocate<Real_t>(numElem));
  sigzz_g.reset(Allocate<Real_t>(numElem));

  determ_g.reset(Allocate<Real_t>(numElem));
  vnewc_g.reset(Allocate<Real_t>(numElem8));

  e_olds_g.reset(Allocate<RealTPtr>(numReg));
  delvcs_g.reset(Allocate<RealTPtr>(numReg));
  p_olds_g.reset(Allocate<RealTPtr>(numReg));
  q_olds_g.reset(Allocate<RealTPtr>(numReg));
  compressions_g.reset(Allocate<RealTPtr>(numReg));
  compHalfSteps_g.reset(Allocate<RealTPtr>(numReg));
  qq_olds_g.reset(Allocate<RealTPtr>(numReg));
  ql_olds_g.reset(Allocate<RealTPtr>(numReg));
  works_g.reset(Allocate<RealTPtr>(numReg));
  p_news_g.reset(Allocate<RealTPtr>(numReg));
  e_news_g.reset(Allocate<RealTPtr>(numReg));
  q_news_g.reset(Allocate<RealTPtr>(numReg));
  bvcs_g.reset(Allocate<RealTPtr>(numReg));
  pbvcs_g.reset(Allocate<RealTPtr>(numReg));
  pHalfSteps_g.reset(Allocate<RealTPtr>(numReg));

  for (std::size_t r = 0; r < numReg; ++r) {
    Index_t regElemSize = domain->regElemSize(r);
    e_olds_g[r].reset(Allocate<Real_t>(regElemSize));
    delvcs_g[r].reset(Allocate<Real_t>(regElemSize));
    p_olds_g[r].reset(Allocate<Real_t>(regElemSize));
    q_olds_g[r].reset(Allocate<Real_t>(regElemSize));
    compressions_g[r].reset(Allocate<Real_t>(regElemSize));
    compHalfSteps_g[r].reset(Allocate<Real_t>(regElemSize));
    qq_olds_g[r].reset(Allocate<Real_t>(regElemSize));
    ql_olds_g[r].reset(Allocate<Real_t>(regElemSize));
    works_g[r].reset(Allocate<Real_t>(regElemSize));
    p_news_g[r].reset(Allocate<Real_t>(regElemSize));
    e_news_g[r].reset(Allocate<Real_t>(regElemSize));
    q_news_g[r].reset(Allocate<Real_t>(regElemSize));
    bvcs_g[r].reset(Allocate<Real_t>(regElemSize));
    pbvcs_g[r].reset(Allocate<Real_t>(regElemSize));
    pHalfSteps_g[r].reset(Allocate<Real_t>(regElemSize));
  }
}
#endif

static inline void CalcHourglassControlForElems(Domain &domain, Real_t determ[],
                                                Real_t hgcoef) {
  Index_t numElem = domain.numElem();
  Index_t numElem8 = numElem * 8;
#ifndef LULESH_ALLOCATE_EXP
  Real_t *dvdx = Allocate<Real_t>(numElem8);
  Real_t *dvdy = Allocate<Real_t>(numElem8);
  Real_t *dvdz = Allocate<Real_t>(numElem8);
  Real_t *x8n = Allocate<Real_t>(numElem8);
  Real_t *y8n = Allocate<Real_t>(numElem8);
  Real_t *z8n = Allocate<Real_t>(numElem8);
#else
  Real_t *dvdx = dvdx_g.get();
  Real_t *dvdy = dvdy_g.get();
  Real_t *dvdz = dvdz_g.get();
  Real_t *x8n = x8n_g.get();
  Real_t *y8n = y8n_g.get();
  Real_t *z8n = z8n_g.get();
#endif

  Domain* domain_ptr = &domain;
  /* start loop over elements */
  std::for_each_n(std::execution::par_unseq, counting_iterator(0), numElem,
                  [=](Index_t i) {
                    Real_t x1[8], y1[8], z1[8];
                    Real_t pfx[8], pfy[8], pfz[8];

                    Index_t *elemToNode = domain_ptr->nodelist(i);
                    CollectDomainNodesToElemNodes(*domain_ptr, elemToNode, x1, y1,
                                                  z1);

                    CalcElemVolumeDerivative(pfx, pfy, pfz, x1, y1, z1);

                    /* load into temporary storage for FB Hour Glass control */
                    for (Index_t ii = 0; ii < 8; ++ii) {
                      Index_t jj = 8 * i + ii;

                      dvdx[jj] = pfx[ii];
                      dvdy[jj] = pfy[ii];
                      dvdz[jj] = pfz[ii];
                      x8n[jj] = x1[ii];
                      y8n[jj] = y1[ii];
                      z8n[jj] = z1[ii];
                    }

                    determ[i] = domain_ptr->volo(i) * domain_ptr->v(i);
                  });
  if (std::any_of(std::execution::par_unseq, domain_ptr->v_begin(), domain_ptr->v_end(),
                  [](Real_t v) { return v < Real_t(0.0); })) {
    exit(VolumeError);
  }

  if (hgcoef > Real_t(0.)) {
    CalcFBHourglassForceForElems(domain, determ, x8n, y8n, z8n, dvdx, dvdy,
                                 dvdz, hgcoef, numElem, domain.numNode());
  }

#ifndef LULESH_ALLOCATE_EXP
  Release(&z8n);
  Release(&y8n);
  Release(&x8n);
  Release(&dvdz);
  Release(&dvdy);
  Release(&dvdx);
#endif

  return;
}

/******************************************/

static inline void CalcVolumeForceForElems(Domain &domain) {
  Index_t numElem = domain.numElem();
  if (numElem != 0) {
    Real_t hgcoef = domain.hgcoef();
#ifndef LULESH_ALLOCATE_EXP
    Real_t *sigxx = Allocate<Real_t>(numElem);
    Real_t *sigyy = Allocate<Real_t>(numElem);
    Real_t *sigzz = Allocate<Real_t>(numElem);
    Real_t *determ = Allocate<Real_t>(numElem);
#else
    Real_t *sigxx = sigxx_g.get();
    Real_t *sigyy = sigyy_g.get();
    Real_t *sigzz = sigzz_g.get();
    Real_t *determ = determ_g.get();
#endif

    /* Sum contributions to total stress tensor */
    InitStressTermsForElems(domain, sigxx, sigyy, sigzz, numElem);

    // call elemlib stress integration loop to produce nodal forces from
    // material stresses.
    IntegrateStressForElems(domain, sigxx, sigyy, sigzz, determ, numElem,
                            domain.numNode());

    // check for negative element volume
    if (std::any_of(std::execution::par_unseq, determ, determ + numElem,
                    [](Real_t value) { return value <= Real_t(0.0); })) {
      exit(VolumeError);
    }

    CalcHourglassControlForElems(domain, determ, hgcoef);

#ifndef LULESH_ALLOCATE_EXP
    Release(&determ);
    Release(&sigzz);
    Release(&sigyy);
    Release(&sigxx);
#endif
  }
}

/******************************************/

static inline void CalcForceForNodes(Domain &domain) {
  Index_t numNode = domain.numNode();

  std::fill(std::execution::par_unseq, domain.fx_begin(), domain.fx_end(),
            Real_t(0.0));
  std::fill(std::execution::par_unseq, domain.fy_begin(), domain.fy_end(),
            Real_t(0.0));
  std::fill(std::execution::par_unseq, domain.fz_begin(), domain.fz_end(),
            Real_t(0.0));

  /* Calcforce calls partial, force, hourq */
  CalcVolumeForceForElems(domain);
}

/******************************************/

static inline void CalcAccelerationForNodes(Domain &domain, Index_t numNode) {
  std::transform(std::execution::par_unseq, domain.fx_begin(), domain.fx_end(),
                 domain.nodalMass_begin(), domain.xdd_begin(),
                 [](Real_t fx, Real_t nodalMass) { return fx / nodalMass; });
  std::transform(std::execution::par_unseq, domain.fy_begin(), domain.fy_end(),
                 domain.nodalMass_begin(), domain.ydd_begin(),
                 [](Real_t fy, Real_t nodalMass) { return fy / nodalMass; });
  std::transform(std::execution::par_unseq, domain.fz_begin(), domain.fz_end(),
                 domain.nodalMass_begin(), domain.zdd_begin(),
                 [](Real_t fz, Real_t nodalMass) { return fz / nodalMass; });
}

/******************************************/

static inline void ApplyAccelerationBoundaryConditionsForNodes(Domain &domain) {
  Index_t size = domain.sizeX();
  Index_t numNodeBC = (size + 1) * (size + 1);

  Domain* domain_ptr = &domain;
  if (!domain.symmXempty()) {
    std::for_each(std::execution::par_unseq, domain.symmX_begin(),
                  domain.symmX_begin() + numNodeBC, [=](Index_t symmX) {
                    domain_ptr->xdd(symmX) = Real_t(0.0);
                  });
  }
  if (!domain.symmYempty()) {
    std::for_each(std::execution::par_unseq, domain.symmY_begin(),
                  domain.symmY_begin() + numNodeBC, [=](Index_t symmY) {
                    domain_ptr->ydd(symmY) = Real_t(0.0);
                  });
  }
  if (!domain.symmZempty()) {
    std::for_each(std::execution::par_unseq, domain.symmZ_begin(),
                  domain.symmZ_begin() + numNodeBC, [=](Index_t symmZ) {
                    domain_ptr->zdd(symmZ) = Real_t(0.0);
                  });
  }
}

/******************************************/

static inline void CalcVelocityForNodes(Domain &domain, const Real_t dt,
                                        const Real_t u_cut, Index_t numNode) {
  std::transform(std::execution::par_unseq, domain.xd_begin(), domain.xd_end(),
                 domain.xdd_begin(), domain.xd_begin(),
                 [=](Real_t xd, Real_t xdd) {
                   Real_t xdnew = xd + xdd * dt;
                   if (std::abs(xdnew) < u_cut)
                     xdnew = Real_t(0.0);
                   return xdnew;
                 });
  std::transform(std::execution::par_unseq, domain.yd_begin(), domain.yd_end(),
                 domain.ydd_begin(), domain.yd_begin(),
                 [=](Real_t yd, Real_t ydd) {
                   Real_t ydnew = yd + ydd * dt;
                   if (std::abs(ydnew) < u_cut)
                     ydnew = Real_t(0.0);
                   return ydnew;
                 });
  std::transform(std::execution::par_unseq, domain.zd_begin(), domain.zd_end(),
                 domain.zdd_begin(), domain.zd_begin(),
                 [=](Real_t zd, Real_t zdd) {
                   Real_t zdnew = zd + zdd * dt;
                   if (std::abs(zdnew) < u_cut)
                     zdnew = Real_t(0.0);
                   return zdnew;
                 });
}

/******************************************/

static inline void CalcPositionForNodes(Domain &domain, const Real_t dt,
                                        Index_t numNode) {
  std::transform(std::execution::par_unseq, domain.x_begin(), domain.x_end(),
                 domain.xd_begin(), domain.x_begin(),
                 [=](Real_t x, Real_t xd) { return x + xd * dt; });
  std::transform(std::execution::par_unseq, domain.y_begin(), domain.y_end(),
                 domain.yd_begin(), domain.y_begin(),
                 [=](Real_t y, Real_t yd) { return y + yd * dt; });
  std::transform(std::execution::par_unseq, domain.z_begin(), domain.z_end(),
                 domain.zd_begin(), domain.z_begin(),
                 [=](Real_t z, Real_t zd) { return z + zd * dt; });
}

/******************************************/

static inline void LagrangeNodal(Domain &domain) {
#ifdef SEDOV_SYNC_POS_VEL_EARLY
  Domain_member fieldData[6];
#endif

  const Real_t delt = domain.deltatime();
  Real_t u_cut = domain.u_cut();

  /* time of boundary condition evaluation is beginning of step for force and
   * acceleration boundary conditions. */
  CalcForceForNodes(domain);

  CalcAccelerationForNodes(domain, domain.numNode());

  ApplyAccelerationBoundaryConditionsForNodes(domain);

  CalcVelocityForNodes(domain, delt, u_cut, domain.numNode());

  CalcPositionForNodes(domain, delt, domain.numNode());

  return;
}

/******************************************/

static inline Real_t CalcElemVolume(
    const Real_t x0, const Real_t x1, const Real_t x2, const Real_t x3,
    const Real_t x4, const Real_t x5, const Real_t x6, const Real_t x7,
    const Real_t y0, const Real_t y1, const Real_t y2, const Real_t y3,
    const Real_t y4, const Real_t y5, const Real_t y6, const Real_t y7,
    const Real_t z0, const Real_t z1, const Real_t z2, const Real_t z3,
    const Real_t z4, const Real_t z5, const Real_t z6, const Real_t z7) {
  Real_t twelveth = Real_t(1.0) / Real_t(12.0);

  Real_t dx61 = x6 - x1;
  Real_t dy61 = y6 - y1;
  Real_t dz61 = z6 - z1;

  Real_t dx70 = x7 - x0;
  Real_t dy70 = y7 - y0;
  Real_t dz70 = z7 - z0;

  Real_t dx63 = x6 - x3;
  Real_t dy63 = y6 - y3;
  Real_t dz63 = z6 - z3;

  Real_t dx20 = x2 - x0;
  Real_t dy20 = y2 - y0;
  Real_t dz20 = z2 - z0;

  Real_t dx50 = x5 - x0;
  Real_t dy50 = y5 - y0;
  Real_t dz50 = z5 - z0;

  Real_t dx64 = x6 - x4;
  Real_t dy64 = y6 - y4;
  Real_t dz64 = z6 - z4;

  Real_t dx31 = x3 - x1;
  Real_t dy31 = y3 - y1;
  Real_t dz31 = z3 - z1;

  Real_t dx72 = x7 - x2;
  Real_t dy72 = y7 - y2;
  Real_t dz72 = z7 - z2;

  Real_t dx43 = x4 - x3;
  Real_t dy43 = y4 - y3;
  Real_t dz43 = z4 - z3;

  Real_t dx57 = x5 - x7;
  Real_t dy57 = y5 - y7;
  Real_t dz57 = z5 - z7;

  Real_t dx14 = x1 - x4;
  Real_t dy14 = y1 - y4;
  Real_t dz14 = z1 - z4;

  Real_t dx25 = x2 - x5;
  Real_t dy25 = y2 - y5;
  Real_t dz25 = z2 - z5;

#define TRIPLE_PRODUCT(x1, y1, z1, x2, y2, z2, x3, y3, z3)                     \
  ((x1) * ((y2) * (z3) - (z2) * (y3)) + (x2) * ((z1) * (y3) - (y1) * (z3)) +   \
   (x3) * ((y1) * (z2) - (z1) * (y2)))

  Real_t volume = TRIPLE_PRODUCT(dx31 + dx72, dx63, dx20, dy31 + dy72, dy63,
                                 dy20, dz31 + dz72, dz63, dz20) +
                  TRIPLE_PRODUCT(dx43 + dx57, dx64, dx70, dy43 + dy57, dy64,
                                 dy70, dz43 + dz57, dz64, dz70) +
                  TRIPLE_PRODUCT(dx14 + dx25, dx61, dx50, dy14 + dy25, dy61,
                                 dy50, dz14 + dz25, dz61, dz50);

#undef TRIPLE_PRODUCT

  volume *= twelveth;

  return volume;
}

/******************************************/

// inline
Real_t CalcElemVolume(const Real_t x[8], const Real_t y[8], const Real_t z[8]) {
  return CalcElemVolume(x[0], x[1], x[2], x[3], x[4], x[5], x[6], x[7], y[0],
                        y[1], y[2], y[3], y[4], y[5], y[6], y[7], z[0], z[1],
                        z[2], z[3], z[4], z[5], z[6], z[7]);
}

/******************************************/

static inline Real_t AreaFace(const Real_t x0, const Real_t x1, const Real_t x2,
                              const Real_t x3, const Real_t y0, const Real_t y1,
                              const Real_t y2, const Real_t y3, const Real_t z0,
                              const Real_t z1, const Real_t z2,
                              const Real_t z3) {
  Real_t fx = (x2 - x0) - (x3 - x1);
  Real_t fy = (y2 - y0) - (y3 - y1);
  Real_t fz = (z2 - z0) - (z3 - z1);
  Real_t gx = (x2 - x0) + (x3 - x1);
  Real_t gy = (y2 - y0) + (y3 - y1);
  Real_t gz = (z2 - z0) + (z3 - z1);
  Real_t area = (fx * fx + fy * fy + fz * fz) * (gx * gx + gy * gy + gz * gz) -
                (fx * gx + fy * gy + fz * gz) * (fx * gx + fy * gy + fz * gz);
  return area;
}

/******************************************/

static inline Real_t CalcElemCharacteristicLength(const Real_t x[8],
                                                  const Real_t y[8],
                                                  const Real_t z[8],
                                                  const Real_t volume) {
  Real_t a, charLength = Real_t(0.0);

  a = AreaFace(x[0], x[1], x[2], x[3], y[0], y[1], y[2], y[3], z[0], z[1], z[2],
               z[3]);
  charLength = std::max(a, charLength);

  a = AreaFace(x[4], x[5], x[6], x[7], y[4], y[5], y[6], y[7], z[4], z[5], z[6],
               z[7]);
  charLength = std::max(a, charLength);

  a = AreaFace(x[0], x[1], x[5], x[4], y[0], y[1], y[5], y[4], z[0], z[1], z[5],
               z[4]);
  charLength = std::max(a, charLength);

  a = AreaFace(x[1], x[2], x[6], x[5], y[1], y[2], y[6], y[5], z[1], z[2], z[6],
               z[5]);
  charLength = std::max(a, charLength);

  a = AreaFace(x[2], x[3], x[7], x[6], y[2], y[3], y[7], y[6], z[2], z[3], z[7],
               z[6]);
  charLength = std::max(a, charLength);

  a = AreaFace(x[3], x[0], x[4], x[7], y[3], y[0], y[4], y[7], z[3], z[0], z[4],
               z[7]);
  charLength = std::max(a, charLength);

  charLength = Real_t(4.0) * volume / SQRT(charLength);

  return charLength;
}

/******************************************/

static inline void
CalcElemVelocityGradient(const Real_t *const xvel, const Real_t *const yvel,
                         const Real_t *const zvel, const Real_t b[][8],
                         const Real_t detJ, Real_t *const d) {
  const Real_t inv_detJ = Real_t(1.0) / detJ;
  Real_t dyddx, dxddy, dzddx, dxddz, dzddy, dyddz;
  const Real_t *const pfx = b[0];
  const Real_t *const pfy = b[1];
  const Real_t *const pfz = b[2];

  d[0] =
      inv_detJ * (pfx[0] * (xvel[0] - xvel[6]) + pfx[1] * (xvel[1] - xvel[7]) +
                  pfx[2] * (xvel[2] - xvel[4]) + pfx[3] * (xvel[3] - xvel[5]));

  d[1] =
      inv_detJ * (pfy[0] * (yvel[0] - yvel[6]) + pfy[1] * (yvel[1] - yvel[7]) +
                  pfy[2] * (yvel[2] - yvel[4]) + pfy[3] * (yvel[3] - yvel[5]));

  d[2] =
      inv_detJ * (pfz[0] * (zvel[0] - zvel[6]) + pfz[1] * (zvel[1] - zvel[7]) +
                  pfz[2] * (zvel[2] - zvel[4]) + pfz[3] * (zvel[3] - zvel[5]));

  dyddx =
      inv_detJ * (pfx[0] * (yvel[0] - yvel[6]) + pfx[1] * (yvel[1] - yvel[7]) +
                  pfx[2] * (yvel[2] - yvel[4]) + pfx[3] * (yvel[3] - yvel[5]));

  dxddy =
      inv_detJ * (pfy[0] * (xvel[0] - xvel[6]) + pfy[1] * (xvel[1] - xvel[7]) +
                  pfy[2] * (xvel[2] - xvel[4]) + pfy[3] * (xvel[3] - xvel[5]));

  dzddx =
      inv_detJ * (pfx[0] * (zvel[0] - zvel[6]) + pfx[1] * (zvel[1] - zvel[7]) +
                  pfx[2] * (zvel[2] - zvel[4]) + pfx[3] * (zvel[3] - zvel[5]));

  dxddz =
      inv_detJ * (pfz[0] * (xvel[0] - xvel[6]) + pfz[1] * (xvel[1] - xvel[7]) +
                  pfz[2] * (xvel[2] - xvel[4]) + pfz[3] * (xvel[3] - xvel[5]));

  dzddy =
      inv_detJ * (pfy[0] * (zvel[0] - zvel[6]) + pfy[1] * (zvel[1] - zvel[7]) +
                  pfy[2] * (zvel[2] - zvel[4]) + pfy[3] * (zvel[3] - zvel[5]));

  dyddz =
      inv_detJ * (pfz[0] * (yvel[0] - yvel[6]) + pfz[1] * (yvel[1] - yvel[7]) +
                  pfz[2] * (yvel[2] - yvel[4]) + pfz[3] * (yvel[3] - yvel[5]));
  d[5] = Real_t(.5) * (dxddy + dyddx);
  d[4] = Real_t(.5) * (dxddz + dzddx);
  d[3] = Real_t(.5) * (dzddy + dyddz);
}

/******************************************/

// static inline
void CalcKinematicsForElems(Domain &domain, Real_t deltaTime, Index_t numElem) {

  Domain* domain_ptr = &domain;
  // loop over all elements
  std::for_each_n(
      std::execution::par_unseq, counting_iterator(0), numElem,
      [=](Index_t k) {
        Real_t B[3][8]; /** shape function derivatives */
        Real_t D[6];
        Real_t x_local[8];
        Real_t y_local[8];
        Real_t z_local[8];
        Real_t xd_local[8];
        Real_t yd_local[8];
        Real_t zd_local[8];
        Real_t detJ = Real_t(0.0);

        Real_t volume;
        Real_t relativeVolume;
        const Index_t *const elemToNode = domain_ptr->nodelist(k);

        // get nodal coordinates from global arrays and copy into local arrays.
        CollectDomainNodesToElemNodes(*domain_ptr, elemToNode, x_local, y_local,
                                      z_local);

        // volume calculations
        volume = CalcElemVolume(x_local, y_local, z_local);
        relativeVolume = volume / domain_ptr->volo(k);
        domain_ptr->vnew(k) = relativeVolume;
        domain_ptr->delv(k) = relativeVolume - domain_ptr->v(k);

        // set characteristic length
        domain_ptr->arealg(k) =
            CalcElemCharacteristicLength(x_local, y_local, z_local, volume);

        // get nodal velocities from global array and copy into local arrays.
        for (Index_t lnode = 0; lnode < 8; ++lnode) {
          Index_t gnode = elemToNode[lnode];
          xd_local[lnode] = domain_ptr->xd(gnode);
          yd_local[lnode] = domain_ptr->yd(gnode);
          zd_local[lnode] = domain_ptr->zd(gnode);
        }

        Real_t dt2 = Real_t(0.5) * deltaTime;
        for (Index_t j = 0; j < 8; ++j) {
          x_local[j] -= dt2 * xd_local[j];
          y_local[j] -= dt2 * yd_local[j];
          z_local[j] -= dt2 * zd_local[j];
        }

        CalcElemShapeFunctionDerivatives(x_local, y_local, z_local, B, &detJ);

        CalcElemVelocityGradient(xd_local, yd_local, zd_local, B, detJ, D);

        // put velocity gradient quantities into their global arrays.
        domain_ptr->dxx(k) = D[0];
        domain_ptr->dyy(k) = D[1];
        domain_ptr->dzz(k) = D[2];
      });
}

/******************************************/

static inline void CalcLagrangeElements(Domain &domain) {
  Index_t numElem = domain.numElem();
  if (numElem > 0) {
    const Real_t deltatime = domain.deltatime();

    domain.AllocateStrains(numElem);

    CalcKinematicsForElems(domain, deltatime, numElem);

    Domain* domain_ptr = &domain;
    // element loop to do some stuff not included in the elemlib function.
    std::for_each_n(std::execution::par_unseq, counting_iterator(0), numElem,
                    [=](Index_t k) {
                      // calc strain rate and apply as constraint (only done in
                      // FB element)
                      Real_t vdov =
                          domain_ptr->dxx(k) + domain_ptr->dyy(k) + domain_ptr->dzz(k);
                      Real_t vdovthird = vdov / Real_t(3.0);

                      // make the rate of deformation tensor deviatoric
                      domain_ptr->vdov(k) = vdov;
                      domain_ptr->dxx(k) -= vdovthird;
                      domain_ptr->dyy(k) -= vdovthird;
                      domain_ptr->dzz(k) -= vdovthird;
                    });
    // See if any volumes are negative, and take appropriate action.
    if (std::any_of(std::execution::par_unseq, domain.vnew_begin(), domain.vnew_end(),
                    [](Real_t vnew) { return vnew <= Real_t(0.0); })) {
      exit(VolumeError);
    }
    domain.DeallocateStrains();
  }
}

/******************************************/

static inline void CalcMonotonicQGradientsForElems(Domain &domain) {
  Index_t numElem = domain.numElem();

  Domain* domain_ptr = &domain;
  std::for_each_n(
      std::execution::par_unseq, counting_iterator(0), numElem, [=](Index_t i) {
        const Real_t ptiny = Real_t(1.e-36);
        Real_t ax, ay, az;
        Real_t dxv, dyv, dzv;

        const Index_t *elemToNode = domain_ptr->nodelist(i);
        Index_t n0 = elemToNode[0];
        Index_t n1 = elemToNode[1];
        Index_t n2 = elemToNode[2];
        Index_t n3 = elemToNode[3];
        Index_t n4 = elemToNode[4];
        Index_t n5 = elemToNode[5];
        Index_t n6 = elemToNode[6];
        Index_t n7 = elemToNode[7];

        Real_t x0 = domain_ptr->x(n0);
        Real_t x1 = domain_ptr->x(n1);
        Real_t x2 = domain_ptr->x(n2);
        Real_t x3 = domain_ptr->x(n3);
        Real_t x4 = domain_ptr->x(n4);
        Real_t x5 = domain_ptr->x(n5);
        Real_t x6 = domain_ptr->x(n6);
        Real_t x7 = domain_ptr->x(n7);

        Real_t y0 = domain_ptr->y(n0);
        Real_t y1 = domain_ptr->y(n1);
        Real_t y2 = domain_ptr->y(n2);
        Real_t y3 = domain_ptr->y(n3);
        Real_t y4 = domain_ptr->y(n4);
        Real_t y5 = domain_ptr->y(n5);
        Real_t y6 = domain_ptr->y(n6);
        Real_t y7 = domain_ptr->y(n7);

        Real_t z0 = domain_ptr->z(n0);
        Real_t z1 = domain_ptr->z(n1);
        Real_t z2 = domain_ptr->z(n2);
        Real_t z3 = domain_ptr->z(n3);
        Real_t z4 = domain_ptr->z(n4);
        Real_t z5 = domain_ptr->z(n5);
        Real_t z6 = domain_ptr->z(n6);
        Real_t z7 = domain_ptr->z(n7);

        Real_t xv0 = domain_ptr->xd(n0);
        Real_t xv1 = domain_ptr->xd(n1);
        Real_t xv2 = domain_ptr->xd(n2);
        Real_t xv3 = domain_ptr->xd(n3);
        Real_t xv4 = domain_ptr->xd(n4);
        Real_t xv5 = domain_ptr->xd(n5);
        Real_t xv6 = domain_ptr->xd(n6);
        Real_t xv7 = domain_ptr->xd(n7);

        Real_t yv0 = domain_ptr->yd(n0);
        Real_t yv1 = domain_ptr->yd(n1);
        Real_t yv2 = domain_ptr->yd(n2);
        Real_t yv3 = domain_ptr->yd(n3);
        Real_t yv4 = domain_ptr->yd(n4);
        Real_t yv5 = domain_ptr->yd(n5);
        Real_t yv6 = domain_ptr->yd(n6);
        Real_t yv7 = domain_ptr->yd(n7);

        Real_t zv0 = domain_ptr->zd(n0);
        Real_t zv1 = domain_ptr->zd(n1);
        Real_t zv2 = domain_ptr->zd(n2);
        Real_t zv3 = domain_ptr->zd(n3);
        Real_t zv4 = domain_ptr->zd(n4);
        Real_t zv5 = domain_ptr->zd(n5);
        Real_t zv6 = domain_ptr->zd(n6);
        Real_t zv7 = domain_ptr->zd(n7);

        Real_t vol = domain_ptr->volo(i) * domain_ptr->vnew(i);
        Real_t norm = Real_t(1.0) / (vol + ptiny);

        Real_t dxj =
            Real_t(-0.25) * ((x0 + x1 + x5 + x4) - (x3 + x2 + x6 + x7));
        Real_t dyj =
            Real_t(-0.25) * ((y0 + y1 + y5 + y4) - (y3 + y2 + y6 + y7));
        Real_t dzj =
            Real_t(-0.25) * ((z0 + z1 + z5 + z4) - (z3 + z2 + z6 + z7));

        Real_t dxi = Real_t(0.25) * ((x1 + x2 + x6 + x5) - (x0 + x3 + x7 + x4));
        Real_t dyi = Real_t(0.25) * ((y1 + y2 + y6 + y5) - (y0 + y3 + y7 + y4));
        Real_t dzi = Real_t(0.25) * ((z1 + z2 + z6 + z5) - (z0 + z3 + z7 + z4));

        Real_t dxk = Real_t(0.25) * ((x4 + x5 + x6 + x7) - (x0 + x1 + x2 + x3));
        Real_t dyk = Real_t(0.25) * ((y4 + y5 + y6 + y7) - (y0 + y1 + y2 + y3));
        Real_t dzk = Real_t(0.25) * ((z4 + z5 + z6 + z7) - (z0 + z1 + z2 + z3));

        /* find delvk and delxk ( i cross j ) */

        ax = dyi * dzj - dzi * dyj;
        ay = dzi * dxj - dxi * dzj;
        az = dxi * dyj - dyi * dxj;

        domain_ptr->delx_zeta(i) = vol / SQRT(ax * ax + ay * ay + az * az + ptiny);

        ax *= norm;
        ay *= norm;
        az *= norm;

        dxv =
            Real_t(0.25) * ((xv4 + xv5 + xv6 + xv7) - (xv0 + xv1 + xv2 + xv3));
        dyv =
            Real_t(0.25) * ((yv4 + yv5 + yv6 + yv7) - (yv0 + yv1 + yv2 + yv3));
        dzv =
            Real_t(0.25) * ((zv4 + zv5 + zv6 + zv7) - (zv0 + zv1 + zv2 + zv3));

        domain_ptr->delv_zeta(i) = ax * dxv + ay * dyv + az * dzv;

        /* find delxi and delvi ( j cross k ) */

        ax = dyj * dzk - dzj * dyk;
        ay = dzj * dxk - dxj * dzk;
        az = dxj * dyk - dyj * dxk;

        domain_ptr->delx_xi(i) = vol / SQRT(ax * ax + ay * ay + az * az + ptiny);

        ax *= norm;
        ay *= norm;
        az *= norm;

        dxv =
            Real_t(0.25) * ((xv1 + xv2 + xv6 + xv5) - (xv0 + xv3 + xv7 + xv4));
        dyv =
            Real_t(0.25) * ((yv1 + yv2 + yv6 + yv5) - (yv0 + yv3 + yv7 + yv4));
        dzv =
            Real_t(0.25) * ((zv1 + zv2 + zv6 + zv5) - (zv0 + zv3 + zv7 + zv4));

        domain_ptr->delv_xi(i) = ax * dxv + ay * dyv + az * dzv;

        /* find delxj and delvj ( k cross i ) */

        ax = dyk * dzi - dzk * dyi;
        ay = dzk * dxi - dxk * dzi;
        az = dxk * dyi - dyk * dxi;

        domain_ptr->delx_eta(i) = vol / SQRT(ax * ax + ay * ay + az * az + ptiny);

        ax *= norm;
        ay *= norm;
        az *= norm;

        dxv =
            Real_t(-0.25) * ((xv0 + xv1 + xv5 + xv4) - (xv3 + xv2 + xv6 + xv7));
        dyv =
            Real_t(-0.25) * ((yv0 + yv1 + yv5 + yv4) - (yv3 + yv2 + yv6 + yv7));
        dzv =
            Real_t(-0.25) * ((zv0 + zv1 + zv5 + zv4) - (zv3 + zv2 + zv6 + zv7));

        domain_ptr->delv_eta(i) = ax * dxv + ay * dyv + az * dzv;
      });
}

/******************************************/

static inline void CalcMonotonicQRegionForElems(Domain &domain, Int_t r,
                                                Real_t ptiny) {
  Real_t monoq_limiter_mult = domain.monoq_limiter_mult();
  Real_t monoq_max_slope = domain.monoq_max_slope();
  Real_t qlc_monoq = domain.qlc_monoq();
  Real_t qqc_monoq = domain.qqc_monoq();

  Domain* domain_ptr = &domain;
  std::for_each_n(
      std::execution::par_unseq, counting_iterator(0), domain.regElemSize(r),
      [=](Index_t i) {
        Index_t ielem = domain_ptr->regElemlist(r, i);
        Real_t qlin, qquad;
        Real_t phixi, phieta, phizeta;
        Int_t bcMask = domain_ptr->elemBC(ielem);
        Real_t delvm = 0.0, delvp = 0.0;

        /*  phixi     */
        Real_t norm = Real_t(1.) / (domain_ptr->delv_xi(ielem) + ptiny);

        switch (bcMask & XI_M) {
        case XI_M_COMM: /* needs comm data */
        case 0:
          delvm = domain_ptr->delv_xi(domain_ptr->lxim(ielem));
          break;
        case XI_M_SYMM:
          delvm = domain_ptr->delv_xi(ielem);
          break;
        case XI_M_FREE:
          delvm = Real_t(0.0);
          break;
        default:
          // // printf("Error in switch at %s line %d\n", __FILE__, __LINE__);
          delvm = 0; /* ERROR - but quiets the compiler */
          break;
        }
        switch (bcMask & XI_P) {
        case XI_P_COMM: /* needs comm data */
        case 0:
          delvp = domain_ptr->delv_xi(domain_ptr->lxip(ielem));
          break;
        case XI_P_SYMM:
          delvp = domain_ptr->delv_xi(ielem);
          break;
        case XI_P_FREE:
          delvp = Real_t(0.0);
          break;
        default:
          // printf("Error in switch at %s line %d\n", __FILE__, __LINE__);
          delvp = 0; /* ERROR - but quiets the compiler */
          break;
        }

        delvm = delvm * norm;
        delvp = delvp * norm;

        phixi = Real_t(.5) * (delvm + delvp);

        delvm *= monoq_limiter_mult;
        delvp *= monoq_limiter_mult;

        if (delvm < phixi)
          phixi = delvm;
        if (delvp < phixi)
          phixi = delvp;
        if (phixi < Real_t(0.))
          phixi = Real_t(0.);
        if (phixi > monoq_max_slope)
          phixi = monoq_max_slope;

        /*  phieta     */
        norm = Real_t(1.) / (domain_ptr->delv_eta(ielem) + ptiny);

        switch (bcMask & ETA_M) {
        case ETA_M_COMM: /* needs comm data */
        case 0:
          delvm = domain_ptr->delv_eta(domain_ptr->letam(ielem));
          break;
        case ETA_M_SYMM:
          delvm = domain_ptr->delv_eta(ielem);
          break;
        case ETA_M_FREE:
          delvm = Real_t(0.0);
          break;
        default:
          // printf("Error in switch at %s line %d\n", __FILE__, __LINE__);
          delvm = 0; /* ERROR - but quiets the compiler */
          break;
        }
        switch (bcMask & ETA_P) {
        case ETA_P_COMM: /* needs comm data */
        case 0:
          delvp = domain_ptr->delv_eta(domain_ptr->letap(ielem));
          break;
        case ETA_P_SYMM:
          delvp = domain_ptr->delv_eta(ielem);
          break;
        case ETA_P_FREE:
          delvp = Real_t(0.0);
          break;
        default:
          // printf("Error in switch at %s line %d\n", __FILE__, __LINE__);
          delvp = 0; /* ERROR - but quiets the compiler */
          break;
        }

        delvm = delvm * norm;
        delvp = delvp * norm;

        phieta = Real_t(.5) * (delvm + delvp);

        delvm *= monoq_limiter_mult;
        delvp *= monoq_limiter_mult;

        if (delvm < phieta)
          phieta = delvm;
        if (delvp < phieta)
          phieta = delvp;
        if (phieta < Real_t(0.))
          phieta = Real_t(0.);
        if (phieta > monoq_max_slope)
          phieta = monoq_max_slope;

        /*  phizeta     */
        norm = Real_t(1.) / (domain_ptr->delv_zeta(ielem) + ptiny);

        switch (bcMask & ZETA_M) {
        case ZETA_M_COMM: /* needs comm data */
        case 0:
          delvm = domain_ptr->delv_zeta(domain_ptr->lzetam(ielem));
          break;
        case ZETA_M_SYMM:
          delvm = domain_ptr->delv_zeta(ielem);
          break;
        case ZETA_M_FREE:
          delvm = Real_t(0.0);
          break;
        default:
          // printf("Error in switch at %s line %d\n", __FILE__, __LINE__);
          delvm = 0; /* ERROR - but quiets the compiler */
          break;
        }
        switch (bcMask & ZETA_P) {
        case ZETA_P_COMM: /* needs comm data */
        case 0:
          delvp = domain_ptr->delv_zeta(domain_ptr->lzetap(ielem));
          break;
        case ZETA_P_SYMM:
          delvp = domain_ptr->delv_zeta(ielem);
          break;
        case ZETA_P_FREE:
          delvp = Real_t(0.0);
          break;
        default:
          // printf("Error in switch at %s line %d\n", __FILE__, __LINE__);
          delvp = 0; /* ERROR - but quiets the compiler */
          break;
        }

        delvm = delvm * norm;
        delvp = delvp * norm;

        phizeta = Real_t(.5) * (delvm + delvp);

        delvm *= monoq_limiter_mult;
        delvp *= monoq_limiter_mult;

        if (delvm < phizeta)
          phizeta = delvm;
        if (delvp < phizeta)
          phizeta = delvp;
        if (phizeta < Real_t(0.))
          phizeta = Real_t(0.);
        if (phizeta > monoq_max_slope)
          phizeta = monoq_max_slope;

        /* Remove length scale */

        if (domain_ptr->vdov(ielem) > Real_t(0.)) {
          qlin = Real_t(0.);
          qquad = Real_t(0.);
        } else {
          Real_t delvxxi = domain_ptr->delv_xi(ielem) * domain_ptr->delx_xi(ielem);
          Real_t delvxeta = domain_ptr->delv_eta(ielem) * domain_ptr->delx_eta(ielem);
          Real_t delvxzeta = domain_ptr->delv_zeta(ielem) * domain_ptr->delx_zeta(ielem);

          if (delvxxi > Real_t(0.))
            delvxxi = Real_t(0.);
          if (delvxeta > Real_t(0.))
            delvxeta = Real_t(0.);
          if (delvxzeta > Real_t(0.))
            delvxzeta = Real_t(0.);

          Real_t rho = domain_ptr->elemMass(ielem) /
                       (domain_ptr->volo(ielem) * domain_ptr->vnew(ielem));

          qlin = -qlc_monoq * rho *
                 (delvxxi * (Real_t(1.) - phixi) +
                  delvxeta * (Real_t(1.) - phieta) +
                  delvxzeta * (Real_t(1.) - phizeta));

          qquad = qqc_monoq * rho *
                  (delvxxi * delvxxi * (Real_t(1.) - phixi * phixi) +
                   delvxeta * delvxeta * (Real_t(1.) - phieta * phieta) +
                   delvxzeta * delvxzeta * (Real_t(1.) - phizeta * phizeta));
        }

        domain_ptr->qq(ielem) = qquad;
        domain_ptr->ql(ielem) = qlin;
      });
}

/******************************************/

static inline void CalcMonotonicQForElems(Domain &domain) {
  //
  // initialize parameters
  //
  const Real_t ptiny = Real_t(1.e-36);

  //
  // calculate the monotonic q for all regions
  //
  for (Index_t r = 0; r < domain.numReg(); ++r) {
    if (domain.regElemSize(r) > 0) {
      CalcMonotonicQRegionForElems(domain, r, ptiny);
    }
  }
}

/******************************************/

static inline void CalcQForElems(Domain &domain) {
  //
  // MONOTONIC Q option
  //

  Index_t numElem = domain.numElem();

  if (numElem != 0) {
    Int_t allElem = numElem +                             /* local elem */
                    2 * domain.sizeX() * domain.sizeY() + /* plane ghosts */
                    2 * domain.sizeX() * domain.sizeZ() + /* row ghosts */
                    2 * domain.sizeY() * domain.sizeZ();  /* col ghosts */

    domain.AllocateGradients(numElem, allElem);

    /* Calculate velocity gradients */
    CalcMonotonicQGradientsForElems(domain);

    CalcMonotonicQForElems(domain);

    // Free up memory
    domain.DeallocateGradients();

    /* Don't allow excessive artificial viscosity */
    Index_t idx = -1;
    for (Index_t i = 0; i < numElem; ++i) {
      if (domain.q(i) > domain.qstop()) {
        idx = i;
        break;
      }
    }

    if (idx >= 0) {
      exit(QStopError);
    }
  }
}

/******************************************/

static inline void CalcPressureForElems(Real_t *p_new, Real_t *bvc,
                                        Real_t *pbvc, Real_t *e_old,
                                        Real_t *compression, Real_t *vnewc,
                                        Real_t pmin, Real_t p_cut,
                                        Real_t eosvmax, Index_t length,
                                        Index_t *regElemList) {
  constexpr Real_t cls = Real_t(2.0) / Real_t(3.0);
  std::transform(std::execution::par_unseq, compression, compression + length, bvc,
                 [=](Real_t compression_i) {
                   return cls * (compression_i + Real_t(1.0));
                 });
  std::fill(std::execution::par_unseq, pbvc, pbvc + length, cls);

  std::for_each_n(
      std::execution::par_unseq, counting_iterator(0), length, [=](Index_t i) {
        Real_t newval = bvc[i] * e_old[i];
        if (std::fabs(newval) < p_cut || vnewc[regElemList[i]] >= eosvmax) {
          newval = Real_t(0.0);
        }
        if (newval < pmin) {
          newval = pmin;
        }
        p_new[i] = newval;
      });
}

/******************************************/

static inline void
CalcEnergyForElems(Real_t *p_new, Real_t *e_new, Real_t *q_new, Real_t *bvc,
                   Real_t *pbvc, Real_t *p_old, Real_t *e_old, Real_t *q_old,
                   Real_t *compression, Real_t *compHalfStep, Real_t *vnewc,
                   Real_t *work, Real_t *delvc, Real_t pmin, Real_t p_cut,
                   Real_t e_cut, Real_t q_cut, Real_t emin, Real_t *qq_old,
                   Real_t *ql_old, Real_t rho0, Real_t eosvmax, Index_t length,
                   Index_t *regElemList, Index_t reg) {
#ifndef LULESH_ALLOCATE_EXP
  Real_t *pHalfStep = Allocate<Real_t>(length);
#else
  Real_t *pHalfStep = pHalfSteps_g[reg].get();
#endif

  std::for_each_n(
      std::execution::par_unseq, counting_iterator(0), length, [=](Index_t i) {
        e_new[i] = e_old[i] - Real_t(0.5) * delvc[i] * (p_old[i] + q_old[i]) +
                   Real_t(0.5) * work[i];

        if (e_new[i] < emin) {
          e_new[i] = emin;
        }
      });

  CalcPressureForElems(pHalfStep, bvc, pbvc, e_new, compHalfStep, vnewc, pmin,
                       p_cut, eosvmax, length, regElemList);

  std::for_each_n(
      std::execution::par_unseq, counting_iterator(0), length, [=](Index_t i) {
        Real_t vhalf = Real_t(1.) / (Real_t(1.) + compHalfStep[i]);

        if (delvc[i] > Real_t(0.)) {
          q_new[i] /* = qq_old[i] = ql_old[i] */ = Real_t(0.);
        } else {
          Real_t ssc =
              (pbvc[i] * e_new[i] + vhalf * vhalf * bvc[i] * pHalfStep[i]) /
              rho0;

          if (ssc <= Real_t(.1111111e-36)) {
            ssc = Real_t(.3333333e-18);
          } else {
            ssc = SQRT(ssc);
          }

          q_new[i] = (ssc * ql_old[i] + qq_old[i]);
        }

        e_new[i] = e_new[i] + Real_t(0.5) * delvc[i] *
                                  (Real_t(3.0) * (p_old[i] + q_old[i]) -
                                   Real_t(4.0) * (pHalfStep[i] + q_new[i]));
      });

  std::transform(std::execution::par_unseq, e_new, e_new + length, work, e_new,
                 [=](Real_t en, Real_t w) {
                   Real_t newval = en + Real_t(0.5) * w;
                   if (std::abs(newval) < e_cut) {
                     newval = Real_t(0.0);
                   }
                   if (newval < emin) {
                     newval = emin;
                   }
                   return newval;
                 });

  CalcPressureForElems(p_new, bvc, pbvc, e_new, compression, vnewc, pmin, p_cut,
                       eosvmax, length, regElemList);

  std::for_each_n(
      std::execution::par_unseq, counting_iterator(0), length, [=](Index_t i) {
        const Real_t sixth = Real_t(1.0) / Real_t(6.0);
        Index_t ielem = regElemList[i];
        Real_t q_tilde;

        if (delvc[i] > Real_t(0.)) {
          q_tilde = Real_t(0.);
        } else {
          Real_t ssc = (pbvc[i] * e_new[i] +
                        vnewc[ielem] * vnewc[ielem] * bvc[i] * p_new[i]) /
                       rho0;

          if (ssc <= Real_t(.1111111e-36)) {
            ssc = Real_t(.3333333e-18);
          } else {
            ssc = SQRT(ssc);
          }

          q_tilde = (ssc * ql_old[i] + qq_old[i]);
        }

        e_new[i] = e_new[i] - (Real_t(7.0) * (p_old[i] + q_old[i]) -
                               Real_t(8.0) * (pHalfStep[i] + q_new[i]) +
                               (p_new[i] + q_tilde)) *
                                  delvc[i] * sixth;

        if (FABS(e_new[i]) < e_cut) {
          e_new[i] = Real_t(0.);
        }
        if (e_new[i] < emin) {
          e_new[i] = emin;
        }
      });

  CalcPressureForElems(p_new, bvc, pbvc, e_new, compression, vnewc, pmin, p_cut,
                       eosvmax, length, regElemList);

  std::for_each_n(
      std::execution::par_unseq, counting_iterator(0), length, [=](Index_t i) {
        Index_t ielem = regElemList[i];

        if (delvc[i] <= Real_t(0.)) {
          Real_t ssc = (pbvc[i] * e_new[i] +
                        vnewc[ielem] * vnewc[ielem] * bvc[i] * p_new[i]) /
                       rho0;

          if (ssc <= Real_t(.1111111e-36)) {
            ssc = Real_t(.3333333e-18);
          } else {
            ssc = SQRT(ssc);
          }

          q_new[i] = (ssc * ql_old[i] + qq_old[i]);

          if (FABS(q_new[i]) < q_cut)
            q_new[i] = Real_t(0.);
        }
      });

#ifndef LULESH_ALLOCATE_EXP
  Release(&pHalfStep);
#endif

  return;
}

/******************************************/

static inline void CalcSoundSpeedForElems(Domain &domain, Real_t *vnewc,
                                          Real_t rho0, Real_t *enewc,
                                          Real_t *pnewc, Real_t *pbvc,
                                          Real_t *bvc, Real_t ss4o3,
                                          Index_t len, Index_t *regElemList) {
  Domain* domain_ptr = &domain;
  std::for_each_n(
      std::execution::par_unseq, counting_iterator(0), len, [=](Index_t i) {
        Index_t ielem = regElemList[i];
        Real_t ssTmp = (pbvc[i] * enewc[i] +
                        vnewc[ielem] * vnewc[ielem] * bvc[i] * pnewc[i]) /
                       rho0;
        if (ssTmp <= Real_t(.1111111e-36)) {
          ssTmp = Real_t(.3333333e-18);
        } else {
          ssTmp = SQRT(ssTmp);
        }
        domain_ptr->ss(ielem) = ssTmp;
      });
}

/******************************************/

static inline void EvalEOSForElems(Domain &domain, Real_t *vnewc,
                                   Int_t numElemReg, Index_t *regElemList,
                                   Int_t rep, Int_t reg) {
  Real_t e_cut = domain.e_cut();
  Real_t p_cut = domain.p_cut();
  Real_t ss4o3 = domain.ss4o3();
  Real_t q_cut = domain.q_cut();

  Real_t eosvmax = domain.eosvmax();
  Real_t eosvmin = domain.eosvmin();
  Real_t pmin = domain.pmin();
  Real_t emin = domain.emin();
  Real_t rho0 = domain.refdens();

  // These temporaries will be of different size for
  // each call (due to different sized region element
  // lists)
#ifndef LULESH_ALLOCATE_EXP
  Real_t *e_old = Allocate<Real_t>(numElemReg);
  Real_t *delvc = Allocate<Real_t>(numElemReg);
  Real_t *p_old = Allocate<Real_t>(numElemReg);
  Real_t *q_old = Allocate<Real_t>(numElemReg);
  Real_t *compression = Allocate<Real_t>(numElemReg);
  Real_t *compHalfStep = Allocate<Real_t>(numElemReg);
  Real_t *qq_old = Allocate<Real_t>(numElemReg);
  Real_t *ql_old = Allocate<Real_t>(numElemReg);
  Real_t *work = Allocate<Real_t>(numElemReg);
  Real_t *p_new = Allocate<Real_t>(numElemReg);
  Real_t *e_new = Allocate<Real_t>(numElemReg);
  Real_t *q_new = Allocate<Real_t>(numElemReg);
  Real_t *bvc = Allocate<Real_t>(numElemReg);
  Real_t *pbvc = Allocate<Real_t>(numElemReg);
#else
  Real_t *e_old = e_olds_g[reg].get();
  Real_t *delvc = delvcs_g[reg].get();
  Real_t *p_old = p_olds_g[reg].get();
  Real_t *q_old = q_olds_g[reg].get();
  Real_t *compression = compressions_g[reg].get();
  Real_t *compHalfStep = compHalfSteps_g[reg].get();
  Real_t *qq_old = qq_olds_g[reg].get();
  Real_t *ql_old = ql_olds_g[reg].get();
  Real_t *work = works_g[reg].get();
  Real_t *p_new = p_news_g[reg].get();
  Real_t *e_new = e_news_g[reg].get();
  Real_t *q_new = q_news_g[reg].get();
  Real_t *bvc = bvcs_g[reg].get();
  Real_t *pbvc = pbvcs_g[reg].get();
#endif

  Domain* domain_ptr = &domain;
  // loop to add load imbalance based on region number
  for (Int_t j = 0; j < rep; j++) {
    /* compress data, minimal set */
    std::for_each_n(std::execution::par_unseq, counting_iterator(0), numElemReg,
                    [=](Index_t i) {
                      Index_t ielem = regElemList[i];
                      e_old[i] = domain_ptr->e(ielem);
                      delvc[i] = domain_ptr->delv(ielem);
                      p_old[i] = domain_ptr->p(ielem);
                      q_old[i] = domain_ptr->q(ielem);
                      qq_old[i] = domain_ptr->qq(ielem);
                      ql_old[i] = domain_ptr->ql(ielem);
                    });

    std::for_each_n(std::execution::par_unseq, counting_iterator(0), numElemReg,
                    [=](Index_t i) {
                      Index_t ielem = regElemList[i];
                      Real_t vchalf;
                      compression[i] = Real_t(1.) / vnewc[ielem] - Real_t(1.);
                      vchalf = vnewc[ielem] - delvc[i] * Real_t(.5);
                      compHalfStep[i] = Real_t(1.) / vchalf - Real_t(1.);
                    });

    /* Check for v > eosvmax or v < eosvmin */
    if (eosvmin != Real_t(0.)) {
      std::for_each_n(std::execution::par_unseq, counting_iterator(0), numElemReg,
                      [=](Index_t i) {
                        Index_t ielem = regElemList[i];
                        if (vnewc[ielem] <=
                            eosvmin) { /* impossible due to calling func? */
                          compHalfStep[i] = compression[i];
                        }
                      });
    }
    if (eosvmax != Real_t(0.)) {
      std::for_each_n(std::execution::par_unseq, counting_iterator(0), numElemReg,
                      [=](Index_t i) {
                        Index_t ielem = regElemList[i];
                        if (vnewc[ielem] >=
                            eosvmax) { /* impossible due to calling func? */
                          p_old[i] = Real_t(0.);
                          compression[i] = Real_t(0.);
                          compHalfStep[i] = Real_t(0.);
                        }
                      });
    }
    std::fill(std::execution::par_unseq, work, work + numElemReg, Real_t(0.0));
    CalcEnergyForElems(p_new, e_new, q_new, bvc, pbvc, p_old, e_old, q_old,
                       compression, compHalfStep, vnewc, work, delvc, pmin,
                       p_cut, e_cut, q_cut, emin, qq_old, ql_old, rho0, eosvmax,
                       numElemReg, regElemList, reg);
  }

  std::for_each_n(std::execution::par_unseq, counting_iterator(0), numElemReg,
                  [=](Index_t i) {
                    Index_t ielem = regElemList[i];
                    domain_ptr->p(ielem) = p_new[i];
                    domain_ptr->e(ielem) = e_new[i];
                    domain_ptr->q(ielem) = q_new[i];
                  });

  CalcSoundSpeedForElems(domain, vnewc, rho0, e_new, p_new, pbvc, bvc, ss4o3,
                         numElemReg, regElemList);

#ifndef LULESH_ALLOCATE_EXP
  Release(&pbvc);
  Release(&bvc);
  Release(&q_new);
  Release(&e_new);
  Release(&p_new);
  Release(&work);
  Release(&ql_old);
  Release(&qq_old);
  Release(&compHalfStep);
  Release(&compression);
  Release(&q_old);
  Release(&p_old);
  Release(&delvc);
  Release(&e_old);
#endif
}

/******************************************/

static inline void ApplyMaterialPropertiesForElems(Domain &domain) {
  Index_t numElem = domain.numElem();

  if (numElem != 0) {
    /* Expose all of the variables needed for material evaluation */
    Real_t eosvmin = domain.eosvmin();
    Real_t eosvmax = domain.eosvmax();
#ifndef LULESH_ALLOCATE_EXP
    Real_t *vnewc = Allocate<Real_t>(numElem);
#else
    Real_t *vnewc = vnewc_g.get();
#endif

    std::copy(std::execution::par_unseq, domain.vnew_begin(), domain.vnew_end(),
              vnewc);

    // Bound the updated relative volumes with eosvmin/max
    if (eosvmin != Real_t(0.)) {
      std::transform(std::execution::par_unseq, vnewc, vnewc + numElem, vnewc,
                     [=](Real_t vc) { return vc < eosvmin ? eosvmin : vc; });
    }

    if (eosvmax != Real_t(0.)) {
      std::transform(std::execution::par_unseq, vnewc, vnewc + numElem, vnewc,
                     [=](Real_t vc) { return vc > eosvmax ? eosvmax : vc; });
    }

    // This check may not make perfect sense in LULESH, but
    // it's representative of something in the full code -
    // just leave it in, please
    if (std::any_of(std::execution::par_unseq, domain.v_begin(), domain.v_end(),
                    [=](Real_t vc) {
                      if (eosvmin != Real_t(0.0) && vc < eosvmin) {
                        vc = eosvmin;
                      }
                      if (eosvmax != Real_t(0.0) && vc > eosvmax) {
                        vc = eosvmax;
                      }
                      return vc < 0.0;
                    })) {
      exit(VolumeError);
    }

    for (Int_t r = 0; r < domain.numReg(); r++) {
      Index_t numElemReg = domain.regElemSize(r);
      Index_t *regElemList = domain.regElemlist(r);
      Int_t rep;
      // Determine load imbalance for this region
      // round down the number with lowest cost
      if (r < domain.numReg() / 2)
        rep = 1;
      // you don't get an expensive region unless you at least have 5 regions
      else if (r < (domain.numReg() - (domain.numReg() + 15) / 20))
        rep = 1 + domain.cost();
      // very expensive regions
      else
        rep = 10 * (1 + domain.cost());
      EvalEOSForElems(domain, vnewc, numElemReg, regElemList, rep, r);
    }

#ifndef LULESH_ALLOCATE_EXP
    Release(&vnewc);
#endif
  }
}

/******************************************/

static inline void UpdateVolumesForElems(Domain &domain, Real_t v_cut,
                                         Index_t length) {
  std::transform(std::execution::par_unseq, domain.vnew_begin(), domain.vnew_end(),
                 domain.v_begin(), [v_cut](Real_t vnew) {
                   if (std::abs(vnew - Real_t(1.0)) < v_cut) {
                     vnew = Real_t(1.0);
                   }
                   return vnew;
                 });
}

/******************************************/

static inline void LagrangeElements(Domain &domain, Index_t numElem) {
  CalcLagrangeElements(domain);

  /* Calculate Q.  (Monotonic q option requires communication) */
  CalcQForElems(domain);

  ApplyMaterialPropertiesForElems(domain);

  UpdateVolumesForElems(domain, domain.v_cut(), numElem);
}

/******************************************/

static inline void CalcCourantConstraintForElems(Domain &domain, Index_t length,
                                                 Index_t *regElemlist,
                                                 Real_t qqc,
                                                 Real_t &dtcourant) {
  Domain* domain_ptr = &domain;
  Real_t qqc2 = Real_t(64.0) * qqc * qqc;
  dtcourant = std::transform_reduce(
      std::execution::par_unseq, counting_iterator(0), counting_iterator(length),
      dtcourant, [](Real_t a, Real_t b) { return a < b ? a : b; },
      [=](Index_t i) {
        Index_t indx = regElemlist[i];
        if (domain_ptr->vdov(indx) == Real_t(0.0)) {
          return std::numeric_limits<Real_t>::max();
        } else {
          Real_t dtf = domain_ptr->ss(indx) * domain_ptr->ss(indx);
          if (domain_ptr->vdov(indx) < Real_t(0.0)) {
            dtf += qqc2 * domain_ptr->arealg(indx) * domain_ptr->arealg(indx) *
                   domain_ptr->vdov(indx) * domain_ptr->vdov(indx);
          }
          dtf = std::sqrt(dtf);
          dtf = domain_ptr->arealg(indx) / dtf;
          return dtf;
        }
      });
}

/******************************************/

static inline void CalcHydroConstraintForElems(Domain &domain, Index_t length,
                                               Index_t *regElemlist,
                                               Real_t dvovmax,
                                               Real_t &dthydro) {
  Domain* domain_ptr = &domain;
  dthydro = std::transform_reduce(
      std::execution::par_unseq, counting_iterator(0), counting_iterator(length),
      dthydro, [](Real_t a, Real_t b) { return a < b ? a : b; },
      [=](Index_t i) {
        Index_t indx = regElemlist[i];
        if (domain_ptr->vdov(indx) == Real_t(0.0)) {
          return std::numeric_limits<Real_t>::max();
        } else {
          return dvovmax / (std::abs(domain_ptr->vdov(indx)) + Real_t(1.e-20));
        }
      });
}

/******************************************/

static inline void CalcTimeConstraintsForElems(Domain &domain) {

  // Initialize conditions to a very large value
  domain.dtcourant() = 1.0e+20;
  domain.dthydro() = 1.0e+20;

  for (Index_t r = 0; r < domain.numReg(); ++r) {
    /* evaluate time constraint */
    CalcCourantConstraintForElems(domain, domain.regElemSize(r),
                                  domain.regElemlist(r), domain.qqc(),
                                  domain.dtcourant());

    /* check hydro constraint */
    CalcHydroConstraintForElems(domain, domain.regElemSize(r),
                                domain.regElemlist(r), domain.dvovmax(),
                                domain.dthydro());
  }
}

/******************************************/

static inline void LagrangeLeapFrog(Domain &domain) {
#ifdef SEDOV_SYNC_POS_VEL_LATE
  Domain_member fieldData[6];
#endif

  /* calculate nodal forces, accelerations, velocities, positions, with
   * applied boundary conditions and slide surface considerations */
#ifdef LULESH_MEASURE_MODULES
  auto start = std::chrono::high_resolution_clock::now();
#endif

  LagrangeNodal(domain);

#ifdef LULESH_MEASURE_MODULES
  auto finish = std::chrono::high_resolution_clock::now();
  std::cout << "LagrangeNodal module time: " << measure(start, finish) << " ms" << std::endl;
  start = std::chrono::high_resolution_clock()::now();
#endif

#ifdef SEDOV_SYNC_POS_VEL_LATE
#endif

  /* calculate element quantities (i.e. velocity gradient & q), and update
   * material states */
  LagrangeElements(domain, domain.numElem());

#ifdef LULESH_MEASURE_MODULES
  finish = std::chrono::high_resolution_clock::now();
  std::cout << "LagrangeElements module time: " << measure(start, finish) << " ms" << std::endl;
  start = std::chrono::high_resolution_clock()::now();
#endif

  CalcTimeConstraintsForElems(domain);

#ifdef LULESH_MEASURE_MODULES
  finish = std::chrono::high_resolution_clock::now();
  std::cout << "CalcTimeConstraintsForElem module time: " << measure(start, finish) << " ms" << std::endl;
  start = std::chrono::high_resolution_clock()::now();
#endif
}

/******************************************/

int main(int argc, char *argv[]) {
  Domain *locDom;
  int numRanks;
  int myRank;
  struct cmdLineOpts opts;

  numRanks = 1;
  myRank = 0;

  /* Set defaults that can be overridden by command line opts */
  opts.its = 9999999;
  opts.nx = 30;
  opts.numReg = 11;
  opts.numFiles = (int)(numRanks + 10) / 9;
  opts.showProg = 0;
  opts.quiet = 0;
  opts.viz = 0;
  opts.balance = 1;
  opts.cost = 1;

  ParseCommandLineOptions(argc, argv, myRank, &opts);

  if ((myRank == 0) && (opts.quiet == 0)) {
    std::cout << "Running problem size " << opts.nx
              << "^3 per domain until completion\n";
    std::cout << "Num processors: " << numRanks << "\n";
#if _OPENMP
    std::cout << "Num threads: " << omp_get_max_threads() << "\n";
#endif
    std::cout << "Total number of elements: "
              << ((Int8_t)numRanks * opts.nx * opts.nx * opts.nx) << " \n\n";
    std::cout << "To run other sizes, use -s <integer>.\n";
    std::cout << "To run a fixed number of iterations, use -i <integer>.\n";
    std::cout
        << "To run a more or less balanced region set, use -b <integer>.\n";
    std::cout << "To change the relative costs of regions, use -c <integer>.\n";
    std::cout << "To print out progress, use -p\n";
    std::cout << "To write an output file for VisIt, use -v\n";
    std::cout << "See help (-h) for more options\n\n";
  }

  // Set up the mesh and decompose. Assumes regular cubes for now
  Int_t col, row, plane, side;
  InitMeshDecomp(numRanks, myRank, &col, &row, &plane, &side);

  // Build the main data structure and initialize it
#ifdef LULESH_USE_SYCL_USM
  locDom = Allocate<Domain>(1);
  ::new(locDom) Domain(numRanks, col, row, plane, opts.nx, side, opts.numReg,
                       opts.balance, opts.cost);
#else
  locDom = new Domain(numRanks, col, row, plane, opts.nx, side, opts.numReg,
                      opts.balance, opts.cost);
#endif

#ifdef LULESH_ALLOCATE_EXP
  allocateGlobals(locDom);
#endif

#ifdef USE_CUDA
   cudaMemAdvise(locDom, sizeof(Domain), cudaMemAdviseSetReadMostly, 0);
#endif

  // BEGIN timestep to solution */
  timeval start;
  gettimeofday(&start, NULL);
  // debug to see region sizes
  //   for(Int_t i = 0; i < locDom->numReg(); i++)
  //      std::cout << "region" << i + 1<< "size" << locDom->regElemSize(i)
  //      <<std::endl;
  while ((locDom->time() < locDom->stoptime()) &&
         (locDom->cycle() < opts.its)) {

    TimeIncrement(*locDom);
    LagrangeLeapFrog(*locDom);

    if ((opts.showProg != 0) && (opts.quiet == 0) && (myRank == 0) && (locDom->cycle() % 100 == 0)) {
      std::cout << "cycle = " << locDom->cycle() << ", " << std::scientific
                << "time = " << double(locDom->time()) << ", "
                << "dt=" << double(locDom->deltatime()) << "\n";
      std::cout.unsetf(std::ios_base::floatfield);
    }
  }

  // Use reduced max elapsed time
  double elapsed_time;
  timeval end;
  gettimeofday(&end, NULL);
  elapsed_time = (double)(end.tv_sec - start.tv_sec) +
                 ((double)(end.tv_usec - start.tv_usec)) / 1000000;
  double elapsed_timeG;
  elapsed_timeG = elapsed_time;

  // Write out final viz file */
  if (opts.viz) {
    DumpToVisit(*locDom, opts.numFiles, myRank, numRanks);
  }

  if ((myRank == 0) && (opts.quiet == 0)) {
    VerifyAndWriteFinalOutput(elapsed_timeG, *locDom, opts.nx, numRanks);
  }

#ifdef LULESH_USE_SYCL_USM
  locDom->~Domain();
  Release(&locDom);
#else
  delete locDom;
#endif

  return 0;
}
