/* SPDX-License-Identifier: BSD-3-Clause
 * Polaris GSL shim -- interop types.
 *
 * A BSD-3 drop-in for the small slice of GSL that astrometry.net links against,
 * so no GPL GSL code enters the build (see docs/LICENSE-AUDIT.md).
 * Layouts match GSL's documented public structs because astrometry.net reads
 * these fields directly (m->size1, v->stride, ...).
 */
#ifndef POLARIS_GSL_TYPES_H
#define POLARIS_GSL_TYPES_H
#include <stddef.h>

typedef struct { size_t size; double* data; } gsl_block;

typedef struct {
    size_t size, stride;
    double* data;
    gsl_block* block;
    int owner;
} gsl_vector;

typedef struct {
    size_t size1, size2, tda;
    double* data;
    gsl_block* block;
    int owner;
} gsl_matrix;

/* GSL declares both the underscored struct tags and the plain typedefs, and
 * astrometry.net uses the underscored spellings (e.g. fit-wcs.c). */
typedef struct { gsl_vector vector; } _gsl_vector_view;
typedef struct { gsl_vector vector; } _gsl_vector_const_view;
typedef struct { gsl_matrix matrix; } _gsl_matrix_view;
typedef struct { gsl_matrix matrix; } _gsl_matrix_const_view;

typedef _gsl_vector_view       gsl_vector_view;
typedef _gsl_vector_const_view gsl_vector_const_view;
typedef _gsl_matrix_view       gsl_matrix_view;
typedef _gsl_matrix_const_view gsl_matrix_const_view;

typedef struct { size_t size; size_t* data; } gsl_permutation;
#endif
