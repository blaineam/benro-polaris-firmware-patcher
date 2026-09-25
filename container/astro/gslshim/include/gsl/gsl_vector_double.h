/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef POLARIS_GSL_VECTOR_DOUBLE_H
#define POLARIS_GSL_VECTOR_DOUBLE_H
#include "gsl_types.h"
gsl_vector* gsl_vector_alloc(size_t n);
gsl_vector* gsl_vector_calloc(size_t n);
void   gsl_vector_free(gsl_vector* v);
double gsl_vector_get(const gsl_vector* v, size_t i);
void   gsl_vector_set(gsl_vector* v, size_t i, double x);
void   gsl_vector_set_zero(gsl_vector* v);
void   gsl_vector_set_all(gsl_vector* v, double x);
gsl_vector_view       gsl_vector_subvector(gsl_vector* v, size_t offset, size_t n);
gsl_vector_view       gsl_vector_view_array(double* base, size_t n);
gsl_vector_const_view gsl_vector_const_view_array(const double* base, size_t n);
#endif
