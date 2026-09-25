/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef POLARIS_GSL_ERRNO_H
#define POLARIS_GSL_ERRNO_H
enum { GSL_SUCCESS = 0, GSL_FAILURE = -1, GSL_EDOM = 1, GSL_ERANGE = 2,
       GSL_EFAULT = 3, GSL_EINVAL = 4, GSL_ENOMEM = 8, GSL_EBADLEN = 19 };
typedef void gsl_error_handler_t(const char* reason, const char* file, int line, int gsl_errno);
gsl_error_handler_t* gsl_set_error_handler(gsl_error_handler_t* new_handler);
gsl_error_handler_t* gsl_set_error_handler_off(void);
const char* gsl_strerror(int gsl_errno);
#endif
