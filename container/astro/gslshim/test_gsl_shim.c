/* SPDX-License-Identifier: BSD-3-Clause
 *
 * Differential test for the Polaris GSL shim.
 *
 * Prints the result of every operation astrometry.net asks of GSL, at full
 * precision. Build it TWICE -- once against the real (GPL) GSL as an oracle,
 * once against our BSD-3 shim -- and diff the output. Identical output means
 * the shim is a faithful replacement. The oracle build is a local test only;
 * no GSL code is shipped.
 */
#include <stdio.h>
#include <math.h>
#include <gsl/gsl_matrix.h>
#include <gsl/gsl_vector.h>
#include <gsl/gsl_permutation.h>
#include <gsl/gsl_linalg.h>
#include <gsl/gsl_blas.h>

static void pm(const char* label, const gsl_matrix* m) {
    size_t i, j;
    printf("%s [%zux%zu]\n", label, m->size1, m->size2);
    for (i = 0; i < m->size1; i++) {
        for (j = 0; j < m->size2; j++) printf("  % .12e", gsl_matrix_get(m, i, j));
        printf("\n");
    }
}
static void pv(const char* label, const gsl_vector* v) {
    size_t i;
    printf("%s [%zu]\n", label, v->size);
    for (i = 0; i < v->size; i++) printf("  % .12e\n", gsl_vector_get(v, i));
}

/* astrometry.net's gslutils_invert_3x3(), verbatim in shape */
static void test_invert_3x3(void) {
    double A[9] = { 2, -1, 0,  -1, 2, -1,  0, -1, 2 };
    double B[9] = { 0 };
    gsl_matrix* LU = gsl_matrix_alloc(3, 3);
    gsl_permutation* p = gsl_permutation_alloc(3);
    gsl_matrix_const_view mA = gsl_matrix_const_view_array(A, 3, 3);
    gsl_matrix_view mB = gsl_matrix_view_array(B, 3, 3);
    int signum;
    gsl_matrix_memcpy(LU, &mA.matrix);
    gsl_linalg_LU_decomp(LU, p, &signum);
    gsl_linalg_LU_invert(LU, p, &mB.matrix);
    printf("== invert_3x3 ==\nsignum %d\n", signum);
    pm("inv", &mB.matrix);
    gsl_permutation_free(p);
    gsl_matrix_free(LU);
}

/* astrometry.net's gslutils_solve_leastsquares(): QR least squares */
static void test_lssolve(void) {
    /* Overdetermined 6x3 -- the shape fit-wcs.c uses for a TAN fit. */
    double a[18] = { 1, 0.10, 0.01,  1, 0.20, 0.04,  1, 0.35, 0.12,
                     1, 0.55, 0.30,  1, 0.72, 0.52,  1, 0.91, 0.83 };
    double bb[6] = { 1.02, 1.31, 1.79, 2.61, 3.35, 4.29 };
    gsl_matrix* A = gsl_matrix_alloc(6, 3);
    gsl_vector* b = gsl_vector_alloc(6);
    gsl_vector* x = gsl_vector_alloc(3);
    gsl_vector* r = gsl_vector_alloc(6);
    gsl_vector* tau = gsl_vector_alloc(3);
    size_t i, j;
    for (i = 0; i < 6; i++) {
        for (j = 0; j < 3; j++) gsl_matrix_set(A, i, j, a[i * 3 + j]);
        gsl_vector_set(b, i, bb[i]);
    }
    gsl_linalg_QR_decomp(A, tau);
    gsl_linalg_QR_lssolve(A, tau, b, x, r);
    printf("== QR lssolve ==\n");
    pv("x", x);
    pv("resid", r);
    gsl_matrix_free(A); gsl_vector_free(b); gsl_vector_free(x);
    gsl_vector_free(r); gsl_vector_free(tau);
}

/* fit-wcs.c's Procrustes rotation: SVD of a 2x2 covariance, then R = V U' */
static void test_svd_2x2(void) {
    double cov[4] = { 0.86602540378, -0.5, 0.5, 0.86602540378 };
    double R[4] = { 0 };
    gsl_matrix* V = gsl_matrix_alloc(2, 2);
    gsl_vector* S = gsl_vector_alloc(2);
    gsl_vector* work = gsl_vector_alloc(2);
    gsl_matrix_view vcov = gsl_matrix_view_array(cov, 2, 2);
    gsl_matrix_view vR = gsl_matrix_view_array(R, 2, 2);
    gsl_matrix* A = &(vcov.matrix);
    gsl_linalg_SV_decomp(A, V, S, work);
    printf("== SVD 2x2 ==\n");
    pv("S", S);
    gsl_blas_dgemm(CblasNoTrans, CblasTrans, 1.0, V, A, 0.0, &(vR.matrix));
    pm("R = V U^T", &(vR.matrix));
    /* rotation must be orthonormal: det +-1, R R^T = I */
    printf("det %.12e\n", R[0] * R[3] - R[1] * R[2]);
    gsl_matrix_free(V); gsl_vector_free(S); gsl_vector_free(work);
}

static void test_dgemm(void) {
    double a[6] = { 1, 2, 3, 4, 5, 6 };      /* 2x3 */
    double b[6] = { 7, 8, 9, 10, 11, 12 };   /* 3x2 */
    double c[4] = { 1, 1, 1, 1 };
    gsl_matrix_view A = gsl_matrix_view_array(a, 2, 3);
    gsl_matrix_view B = gsl_matrix_view_array(b, 3, 2);
    gsl_matrix_view C = gsl_matrix_view_array(c, 2, 2);
    printf("== dgemm ==\n");
    gsl_blas_dgemm(CblasNoTrans, CblasNoTrans, 2.0, &A.matrix, &B.matrix, 3.0, &C.matrix);
    pm("2*A*B + 3*C", &C.matrix);
    {   /* transposed variants */
        double d[9] = { 0 };
        gsl_matrix_view D = gsl_matrix_view_array(d, 3, 3);
        gsl_blas_dgemm(CblasTrans, CblasTrans, 1.0, &A.matrix, &B.matrix, 0.0, &D.matrix);
        pm("A^T * B^T", &D.matrix);
    }
}

int main(void) {
    test_invert_3x3();
    test_lssolve();
    test_svd_2x2();
    test_dgemm();
    return 0;
}
