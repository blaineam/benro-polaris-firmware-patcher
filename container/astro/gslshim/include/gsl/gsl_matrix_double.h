/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef POLARIS_GSL_MATRIX_DOUBLE_H
#define POLARIS_GSL_MATRIX_DOUBLE_H
#include "gsl_types.h"
gsl_matrix* gsl_matrix_alloc(size_t n1, size_t n2);
gsl_matrix* gsl_matrix_calloc(size_t n1, size_t n2);
void   gsl_matrix_free(gsl_matrix* m);
double gsl_matrix_get(const gsl_matrix* m, size_t i, size_t j);
void   gsl_matrix_set(gsl_matrix* m, size_t i, size_t j, double x);
void   gsl_matrix_set_zero(gsl_matrix* m);
void   gsl_matrix_set_identity(gsl_matrix* m);
int    gsl_matrix_memcpy(gsl_matrix* dest, const gsl_matrix* src);
gsl_matrix_view       gsl_matrix_view_array(double* base, size_t n1, size_t n2);
gsl_matrix_const_view gsl_matrix_const_view_array(const double* base, size_t n1, size_t n2);
gsl_matrix_view       gsl_matrix_submatrix(gsl_matrix* m, size_t i, size_t j, size_t n1, size_t n2);
#endif
