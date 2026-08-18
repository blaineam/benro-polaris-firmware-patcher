/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef POLARIS_GSL_BLAS_H
#define POLARIS_GSL_BLAS_H
#include "gsl_types.h"
typedef enum { CblasNoTrans = 111, CblasTrans = 112, CblasConjTrans = 113 } CBLAS_TRANSPOSE_t;
int gsl_blas_dgemm(CBLAS_TRANSPOSE_t TransA, CBLAS_TRANSPOSE_t TransB,
                   double alpha, const gsl_matrix* A, const gsl_matrix* B,
                   double beta, gsl_matrix* C);
#endif
