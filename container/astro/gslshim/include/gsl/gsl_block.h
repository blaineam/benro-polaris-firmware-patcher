/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef POLARIS_GSL_BLOCK_H
#define POLARIS_GSL_BLOCK_H
#include "gsl_types.h"
gsl_block* gsl_block_alloc(size_t n);
gsl_block* gsl_block_calloc(size_t n);
void gsl_block_free(gsl_block* b);
#endif
