/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef POLARIS_GSL_PERMUTATION_H
#define POLARIS_GSL_PERMUTATION_H
#include "gsl_types.h"
gsl_permutation* gsl_permutation_alloc(size_t n);
gsl_permutation* gsl_permutation_calloc(size_t n);
void gsl_permutation_free(gsl_permutation* p);
#endif
