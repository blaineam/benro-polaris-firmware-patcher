/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef POLARIS_GSL_LINALG_H
#define POLARIS_GSL_LINALG_H
#include "gsl_types.h"
int gsl_linalg_LU_decomp(gsl_matrix* A, gsl_permutation* p, int* signum);
int gsl_linalg_LU_invert(const gsl_matrix* LU, const gsl_permutation* p, gsl_matrix* inverse);
int gsl_linalg_QR_decomp(gsl_matrix* A, gsl_vector* tau);
int gsl_linalg_QR_lssolve(const gsl_matrix* QR, const gsl_vector* tau,
                          const gsl_vector* b, gsl_vector* x, gsl_vector* residual);
int gsl_linalg_SV_decomp(gsl_matrix* A, gsl_matrix* V, gsl_vector* S, gsl_vector* work);
int gsl_linalg_SV_decomp_jacobi(gsl_matrix* A, gsl_matrix* V, gsl_vector* S);
#endif
