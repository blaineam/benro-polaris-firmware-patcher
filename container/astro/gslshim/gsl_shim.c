/* SPDX-License-Identifier: BSD-3-Clause
 *
 * Polaris GSL shim — a BSD-3 replacement for the slice of GSL that
 * astrometry.net links against, so the GPL-v3 `gsl-an` tree never enters the
 * build (see docs/LICENSE-AUDIT.md).
 *
 * Same idea as Aperion's Accelerate-backed shim, but this one is dependency
 * free: no BLAS, no LAPACK, no Accelerate. That is deliberate — astrometry.net
 * only ever calls these on tiny matrices:
 *
 *   gslutils_invert_3x3()          LU on 3x3
 *   gslutils_solve_leastsquares()  QR least squares, M x N with N small
 *   fit-wcs.c (Procrustes fit)     SVD on 2x2, plus dgemm on 2x2
 *
 * so a straightforward Householder QR / partial-pivot LU / one-sided Jacobi SVD
 * is both accurate enough and smaller and faster here than pulling in a BLAS.
 *
 * Row-major throughout, matching GSL (m->data[i*m->tda + j]).
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include <gsl/gsl_block.h>
#include <gsl/gsl_vector.h>
#include <gsl/gsl_matrix.h>
#include <gsl/gsl_permutation.h>
#include <gsl/gsl_linalg.h>
#include <gsl/gsl_blas.h>
#include <gsl/gsl_errno.h>

#define MREF(m,i,j) ((m)->data[(i) * (m)->tda + (j)])
#define VREF(v,i)   ((v)->data[(i) * (v)->stride])

/* ---------------------------------------------------------------- block */

gsl_block* gsl_block_alloc(size_t n) {
    gsl_block* b;
    if (!n) return NULL;
    b = (gsl_block*)malloc(sizeof(gsl_block));
    if (!b) return NULL;
    b->size = n;
    b->data = (double*)malloc(n * sizeof(double));
    if (!b->data) { free(b); return NULL; }
    return b;
}
gsl_block* gsl_block_calloc(size_t n) {
    gsl_block* b = gsl_block_alloc(n);
    if (b) memset(b->data, 0, n * sizeof(double));
    return b;
}
void gsl_block_free(gsl_block* b) {
    if (!b) return;
    free(b->data);
    free(b);
}

/* --------------------------------------------------------------- vector */

gsl_vector* gsl_vector_alloc(size_t n) {
    gsl_vector* v;
    gsl_block* b;
    if (!n) return NULL;
    b = gsl_block_alloc(n);
    if (!b) return NULL;
    v = (gsl_vector*)malloc(sizeof(gsl_vector));
    if (!v) { gsl_block_free(b); return NULL; }
    v->size = n; v->stride = 1; v->data = b->data; v->block = b; v->owner = 1;
    return v;
}
gsl_vector* gsl_vector_calloc(size_t n) {
    gsl_vector* v = gsl_vector_alloc(n);
    if (v) memset(v->data, 0, n * sizeof(double));
    return v;
}
void gsl_vector_free(gsl_vector* v) {
    if (!v) return;
    if (v->owner && v->block) gsl_block_free(v->block);
    free(v);
}
double gsl_vector_get(const gsl_vector* v, size_t i) { return VREF(v, i); }
void   gsl_vector_set(gsl_vector* v, size_t i, double x) { VREF(v, i) = x; }
void   gsl_vector_set_zero(gsl_vector* v) {
    size_t i; for (i = 0; i < v->size; i++) VREF(v, i) = 0.0;
}
void   gsl_vector_set_all(gsl_vector* v, double x) {
    size_t i; for (i = 0; i < v->size; i++) VREF(v, i) = x;
}
gsl_vector_view gsl_vector_subvector(gsl_vector* v, size_t offset, size_t n) {
    gsl_vector_view view;
    view.vector.size = n; view.vector.stride = v->stride;
    view.vector.data = v->data + offset * v->stride;
    view.vector.block = NULL; view.vector.owner = 0;
    return view;
}
gsl_vector_view gsl_vector_view_array(double* base, size_t n) {
    gsl_vector_view view;
    view.vector.size = n; view.vector.stride = 1; view.vector.data = base;
    view.vector.block = NULL; view.vector.owner = 0;
    return view;
}
gsl_vector_const_view gsl_vector_const_view_array(const double* base, size_t n) {
    gsl_vector_const_view view;
    view.vector.size = n; view.vector.stride = 1;
    view.vector.data = (double*)base;   /* const-ness is the caller's contract */
    view.vector.block = NULL; view.vector.owner = 0;
    return view;
}

/* --------------------------------------------------------------- matrix */

gsl_matrix* gsl_matrix_alloc(size_t n1, size_t n2) {
    gsl_matrix* m;
    gsl_block* b;
    if (!n1 || !n2) return NULL;
    b = gsl_block_alloc(n1 * n2);
    if (!b) return NULL;
    m = (gsl_matrix*)malloc(sizeof(gsl_matrix));
    if (!m) { gsl_block_free(b); return NULL; }
    m->size1 = n1; m->size2 = n2; m->tda = n2;
    m->data = b->data; m->block = b; m->owner = 1;
    return m;
}
gsl_matrix* gsl_matrix_calloc(size_t n1, size_t n2) {
    gsl_matrix* m = gsl_matrix_alloc(n1, n2);
    if (m) memset(m->data, 0, n1 * n2 * sizeof(double));
    return m;
}
void gsl_matrix_free(gsl_matrix* m) {
    if (!m) return;
    if (m->owner && m->block) gsl_block_free(m->block);
    free(m);
}
double gsl_matrix_get(const gsl_matrix* m, size_t i, size_t j) { return MREF(m, i, j); }
void   gsl_matrix_set(gsl_matrix* m, size_t i, size_t j, double x) { MREF(m, i, j) = x; }
void   gsl_matrix_set_zero(gsl_matrix* m) {
    size_t i, j;
    for (i = 0; i < m->size1; i++) for (j = 0; j < m->size2; j++) MREF(m, i, j) = 0.0;
}
void   gsl_matrix_set_identity(gsl_matrix* m) {
    size_t i, j;
    for (i = 0; i < m->size1; i++)
        for (j = 0; j < m->size2; j++) MREF(m, i, j) = (i == j) ? 1.0 : 0.0;
}
int gsl_matrix_memcpy(gsl_matrix* dest, const gsl_matrix* src) {
    size_t i, j;
    if (dest->size1 != src->size1 || dest->size2 != src->size2) return GSL_EBADLEN;
    for (i = 0; i < src->size1; i++)
        for (j = 0; j < src->size2; j++) MREF(dest, i, j) = MREF(src, i, j);
    return GSL_SUCCESS;
}
gsl_matrix_view gsl_matrix_view_array(double* base, size_t n1, size_t n2) {
    gsl_matrix_view view;
    view.matrix.size1 = n1; view.matrix.size2 = n2; view.matrix.tda = n2;
    view.matrix.data = base; view.matrix.block = NULL; view.matrix.owner = 0;
    return view;
}
gsl_matrix_const_view gsl_matrix_const_view_array(const double* base, size_t n1, size_t n2) {
    gsl_matrix_const_view view;
    view.matrix.size1 = n1; view.matrix.size2 = n2; view.matrix.tda = n2;
    view.matrix.data = (double*)base; view.matrix.block = NULL; view.matrix.owner = 0;
    return view;
}
gsl_matrix_view gsl_matrix_submatrix(gsl_matrix* m, size_t i, size_t j,
                                     size_t n1, size_t n2) {
    gsl_matrix_view view;
    view.matrix.size1 = n1; view.matrix.size2 = n2; view.matrix.tda = m->tda;
    view.matrix.data = m->data + i * m->tda + j;
    view.matrix.block = NULL; view.matrix.owner = 0;
    return view;
}

/* ---------------------------------------------------------- permutation */

gsl_permutation* gsl_permutation_alloc(size_t n) {
    gsl_permutation* p;
    size_t i;
    if (!n) return NULL;
    p = (gsl_permutation*)malloc(sizeof(gsl_permutation));
    if (!p) return NULL;
    p->size = n;
    p->data = (size_t*)malloc(n * sizeof(size_t));
    if (!p->data) { free(p); return NULL; }
    for (i = 0; i < n; i++) p->data[i] = i;   /* GSL's alloc() is the identity */
    return p;
}
gsl_permutation* gsl_permutation_calloc(size_t n) { return gsl_permutation_alloc(n); }
void gsl_permutation_free(gsl_permutation* p) {
    if (!p) return;
    free(p->data);
    free(p);
}

/* ------------------------------------------------------------- LU / inv */

int gsl_linalg_LU_decomp(gsl_matrix* A, gsl_permutation* p, int* signum) {
    size_t n, i, j, k;
    if (!A || A->size1 != A->size2) return GSL_EBADLEN;
    n = A->size1;
    if (!p || p->size != n) return GSL_EBADLEN;
    for (i = 0; i < n; i++) p->data[i] = i;
    *signum = 1;
    for (k = 0; k + 1 < n; k++) {
        size_t piv = k;
        double amax = fabs(MREF(A, k, k));
        for (i = k + 1; i < n; i++) {
            double a = fabs(MREF(A, i, k));
            if (a > amax) { amax = a; piv = i; }
        }
        if (piv != k) {
            size_t t;
            for (j = 0; j < n; j++) {
                double tmp = MREF(A, k, j); MREF(A, k, j) = MREF(A, piv, j); MREF(A, piv, j) = tmp;
            }
            t = p->data[k]; p->data[k] = p->data[piv]; p->data[piv] = t;
            *signum = -*signum;
        }
        if (MREF(A, k, k) != 0.0) {
            for (i = k + 1; i < n; i++) {
                double f = MREF(A, i, k) / MREF(A, k, k);
                MREF(A, i, k) = f;
                for (j = k + 1; j < n; j++) MREF(A, i, j) -= f * MREF(A, k, j);
            }
        }
    }
    return GSL_SUCCESS;
}

/* Solve LU x = P b for one column. */
static void lu_solve_col(const gsl_matrix* LU, const gsl_permutation* p,
                         const double* b, double* x, size_t n) {
    size_t i, j;
    for (i = 0; i < n; i++) {                    /* forward: unit lower */
        double s = b[p->data[i]];
        for (j = 0; j < i; j++) s -= MREF(LU, i, j) * x[j];
        x[i] = s;
    }
    for (i = n; i-- > 0; ) {                     /* back: upper */
        double s = x[i];
        for (j = i + 1; j < n; j++) s -= MREF(LU, i, j) * x[j];
        x[i] = (MREF(LU, i, i) == 0.0) ? 0.0 : s / MREF(LU, i, i);
    }
}

int gsl_linalg_LU_invert(const gsl_matrix* LU, const gsl_permutation* p,
                         gsl_matrix* inverse) {
    size_t n, i, k;
    double *e, *x;
    if (!LU || LU->size1 != LU->size2) return GSL_EBADLEN;
    n = LU->size1;
    if (!inverse || inverse->size1 != n || inverse->size2 != n) return GSL_EBADLEN;
    for (i = 0; i < n; i++)
        if (MREF(LU, i, i) == 0.0) return GSL_EDOM;   /* singular */
    e = (double*)calloc(n, sizeof(double));
    x = (double*)calloc(n, sizeof(double));
    if (!e || !x) { free(e); free(x); return GSL_ENOMEM; }
    for (k = 0; k < n; k++) {
        memset(e, 0, n * sizeof(double));
        e[k] = 1.0;
        lu_solve_col(LU, p, e, x, n);
        for (i = 0; i < n; i++) MREF(inverse, i, k) = x[i];
    }
    free(e); free(x);
    return GSL_SUCCESS;
}

/* --------------------------------------------------- QR + least squares */
/* Householder QR, GSL-compatible packing: R in the upper triangle of A, the
 * essential part of each Householder vector below the diagonal, tau[j] the
 * corresponding scalar. */

int gsl_linalg_QR_decomp(gsl_matrix* A, gsl_vector* tau) {
    size_t M, N, i, j, k, kmax;
    if (!A || !tau) return GSL_EFAULT;
    M = A->size1; N = A->size2;
    kmax = (M < N) ? M : N;
    if (tau->size != kmax) return GSL_EBADLEN;
    for (k = 0; k < kmax; k++) {
        double norm = 0.0, alpha, beta, tauk;
        for (i = k; i < M; i++) norm += MREF(A, i, k) * MREF(A, i, k);
        norm = sqrt(norm);
        alpha = MREF(A, k, k);
        if (norm == 0.0) { VREF(tau, k) = 0.0; continue; }
        beta = (alpha >= 0.0) ? -norm : norm;
        tauk = (beta - alpha) / beta;
        /* v = x / (alpha - beta), v[k] = 1 implicitly */
        {
            double d = alpha - beta;
            for (i = k + 1; i < M; i++) MREF(A, i, k) /= d;
        }
        MREF(A, k, k) = beta;
        VREF(tau, k) = tauk;
        for (j = k + 1; j < N; j++) {           /* apply H to trailing cols */
            double s = MREF(A, k, j);
            for (i = k + 1; i < M; i++) s += MREF(A, i, k) * MREF(A, i, j);
            s *= tauk;
            MREF(A, k, j) -= s;
            for (i = k + 1; i < M; i++) MREF(A, i, j) -= s * MREF(A, i, k);
        }
    }
    return GSL_SUCCESS;
}

int gsl_linalg_QR_lssolve(const gsl_matrix* QR, const gsl_vector* tau,
                          const gsl_vector* b, gsl_vector* x,
                          gsl_vector* residual) {
    size_t M, N, i, j, k, kmax;
    double* w;
    if (!QR || !tau || !b || !x) return GSL_EFAULT;
    M = QR->size1; N = QR->size2;
    if (b->size != M || x->size != N) return GSL_EBADLEN;
    if (residual && residual->size != M) return GSL_EBADLEN;
    kmax = (M < N) ? M : N;
    w = (double*)malloc(M * sizeof(double));
    if (!w) return GSL_ENOMEM;
    for (i = 0; i < M; i++) w[i] = VREF(b, i);
    for (k = 0; k < kmax; k++) {                 /* w = Q^T b */
        double s = w[k], t = VREF(tau, k);
        if (t == 0.0) continue;
        for (i = k + 1; i < M; i++) s += MREF(QR, i, k) * w[i];
        s *= t;
        w[k] -= s;
        for (i = k + 1; i < M; i++) w[i] -= s * MREF(QR, i, k);
    }
    for (i = N; i-- > 0; ) {                     /* back-substitute R x = w */
        double s = w[i];
        for (j = i + 1; j < N; j++) s -= MREF(QR, i, j) * VREF(x, j);
        VREF(x, i) = (MREF(QR, i, i) == 0.0) ? 0.0 : s / MREF(QR, i, i);
    }
    if (residual) {                              /* r = Q [0...0, w_N..w_M] */
        for (i = 0; i < N && i < M; i++) w[i] = 0.0;
        for (k = kmax; k-- > 0; ) {
            double s = w[k], t = VREF(tau, k);
            if (t == 0.0) continue;
            for (i = k + 1; i < M; i++) s += MREF(QR, i, k) * w[i];
            s *= t;
            w[k] -= s;
            for (i = k + 1; i < M; i++) w[i] -= s * MREF(QR, i, k);
        }
        for (i = 0; i < M; i++) VREF(residual, i) = w[i];
    }
    free(w);
    return GSL_SUCCESS;
}

/* ------------------------------------------------------------------ SVD */
/* One-sided Jacobi: accurate for the small, well-conditioned matrices
 * astrometry.net feeds it (2x2 in fit-wcs.c), and it needs no bidiagonal
 * reduction. A is overwritten with U, V holds the right singular vectors,
 * S the singular values in descending order. */

static int svd_jacobi(gsl_matrix* A, gsl_matrix* V, gsl_vector* S) {
    size_t M = A->size1, N = A->size2, i, j, k, sweep;
    if (V->size1 != N || V->size2 != N || S->size != N) return GSL_EBADLEN;
    gsl_matrix_set_identity(V);
    for (sweep = 0; sweep < 60; sweep++) {
        double off = 0.0;
        for (j = 0; j + 1 < N; j++) {
            for (k = j + 1; k < N; k++) {
                double a = 0.0, b = 0.0, c = 0.0, t, tau_, cs, sn;
                for (i = 0; i < M; i++) {
                    double aij = MREF(A, i, j), aik = MREF(A, i, k);
                    a += aij * aij; b += aik * aik; c += aij * aik;
                }
                off += fabs(c);
                if (fabs(c) < 1e-300) continue;
                tau_ = (b - a) / (2.0 * c);
                t = (tau_ >= 0.0) ? 1.0 / (tau_ + sqrt(1.0 + tau_ * tau_))
                                  : -1.0 / (-tau_ + sqrt(1.0 + tau_ * tau_));
                cs = 1.0 / sqrt(1.0 + t * t);
                sn = cs * t;
                for (i = 0; i < M; i++) {
                    double aij = MREF(A, i, j), aik = MREF(A, i, k);
                    MREF(A, i, j) = cs * aij - sn * aik;
                    MREF(A, i, k) = sn * aij + cs * aik;
                }
                for (i = 0; i < N; i++) {
                    double vij = MREF(V, i, j), vik = MREF(V, i, k);
                    MREF(V, i, j) = cs * vij - sn * vik;
                    MREF(V, i, k) = sn * vij + cs * vik;
                }
            }
        }
        if (off < 1e-15) break;
    }
    for (j = 0; j < N; j++) {                    /* singular values + normalise U */
        double norm = 0.0;
        for (i = 0; i < M; i++) norm += MREF(A, i, j) * MREF(A, i, j);
        norm = sqrt(norm);
        VREF(S, j) = norm;
        if (norm > 0.0) for (i = 0; i < M; i++) MREF(A, i, j) /= norm;
    }
    for (j = 0; j + 1 < N; j++) {                /* sort descending */
        size_t best = j;
        for (k = j + 1; k < N; k++) if (VREF(S, k) > VREF(S, best)) best = k;
        if (best == j) continue;
        { double t = VREF(S, j); VREF(S, j) = VREF(S, best); VREF(S, best) = t; }
        for (i = 0; i < M; i++) {
            double t = MREF(A, i, j); MREF(A, i, j) = MREF(A, i, best); MREF(A, i, best) = t;
        }
        for (i = 0; i < N; i++) {
            double t = MREF(V, i, j); MREF(V, i, j) = MREF(V, i, best); MREF(V, i, best) = t;
        }
    }
    return GSL_SUCCESS;
}

int gsl_linalg_SV_decomp(gsl_matrix* A, gsl_matrix* V, gsl_vector* S,
                         gsl_vector* work) {
    (void)work;                                  /* not needed by this method */
    return svd_jacobi(A, V, S);
}
int gsl_linalg_SV_decomp_jacobi(gsl_matrix* A, gsl_matrix* V, gsl_vector* S) {
    return svd_jacobi(A, V, S);
}

/* ----------------------------------------------------------------- BLAS */

int gsl_blas_dgemm(CBLAS_TRANSPOSE_t TransA, CBLAS_TRANSPOSE_t TransB,
                   double alpha, const gsl_matrix* A, const gsl_matrix* B,
                   double beta, gsl_matrix* C) {
    size_t M = C->size1, N = C->size2, K, i, j, k;
    K = (TransA == CblasNoTrans) ? A->size2 : A->size1;
    if ((TransA == CblasNoTrans ? A->size1 : A->size2) != M) return GSL_EBADLEN;
    if ((TransB == CblasNoTrans ? B->size2 : B->size1) != N) return GSL_EBADLEN;
    if ((TransB == CblasNoTrans ? B->size1 : B->size2) != K) return GSL_EBADLEN;
    for (i = 0; i < M; i++) {
        for (j = 0; j < N; j++) {
            double s = 0.0;
            for (k = 0; k < K; k++) {
                double a = (TransA == CblasNoTrans) ? MREF(A, i, k) : MREF(A, k, i);
                double b = (TransB == CblasNoTrans) ? MREF(B, k, j) : MREF(B, j, k);
                s += a * b;
            }
            MREF(C, i, j) = alpha * s + beta * MREF(C, i, j);
        }
    }
    return GSL_SUCCESS;
}

/* ---------------------------------------------------------------- errno */

static gsl_error_handler_t* the_handler = NULL;

gsl_error_handler_t* gsl_set_error_handler(gsl_error_handler_t* new_handler) {
    gsl_error_handler_t* old = the_handler;
    the_handler = new_handler;
    return old;
}
gsl_error_handler_t* gsl_set_error_handler_off(void) {
    gsl_error_handler_t* old = the_handler;
    the_handler = NULL;
    return old;
}
const char* gsl_strerror(int gsl_errno) {
    switch (gsl_errno) {
        case GSL_SUCCESS: return "success";
        case GSL_FAILURE: return "failure";
        case GSL_EDOM:    return "input domain error";
        case GSL_ERANGE:  return "output range error";
        case GSL_EFAULT:  return "invalid pointer";
        case GSL_EINVAL:  return "invalid argument";
        case GSL_ENOMEM:  return "malloc failed";
        case GSL_EBADLEN: return "matrix/vector sizes are not conformant";
        default:          return "unknown error";
    }
}
