/*
    -- MAGMA (version 1.0) --
       Univ. of Tennessee, Knoxville
       Univ. of California, Berkeley
       Univ. of Colorado, Denver
       November 2010

       @generated c

*/
#include "common_magma.h"

extern "C" magma_int_t
magma_cgeqrs_gpu(magma_int_t m, magma_int_t n, magma_int_t nrhs,
		 cuFloatComplex *dA,    magma_int_t ldda, 
		 cuFloatComplex *tau,   cuFloatComplex *dT, 
                 cuFloatComplex *dB,    magma_int_t lddb, 
		 cuFloatComplex *hwork, magma_int_t lwork, 
                 magma_int_t *info)
{
/*  -- MAGMA (version 1.0) --
       Univ. of Tennessee, Knoxville
       Univ. of California, Berkeley
       Univ. of Colorado, Denver
       November 2010

    Purpose
    =======
    Solves the least squares problem
           min || A*X - C ||
    using the QR factorization A = Q*R computed by CGEQRF_GPU.

    Arguments
    =========
    M       (input) INTEGER
            The number of rows of the matrix A. M >= 0.

    N       (input) INTEGER
            The number of columns of the matrix A. M >= N >= 0.

    NRHS    (input) INTEGER
            The number of columns of the matrix C. NRHS >= 0.

    A       (input) COMPLEX array on the GPU, dimension (LDDA,N)
            The i-th column must contain the vector which defines the
            elementary reflector H(i), for i = 1,2,...,n, as returned by
            CGEQRF_GPU2 in the first n columns of its array argument A.

    LDDA    (input) INTEGER
            The leading dimension of the array A, LDDA >= M.

    TAU     (input) COMPLEX array, dimension (N)
            TAU(i) must contain the scalar factor of the elementary
            reflector H(i), as returned by MAGMA_CGEQRF_GPU.

    DB      (input/output) COMPLEX array on the GPU, dimension (LDDB,NRHS)
            On entry, the M-by-NRHS matrix C.
            On exit, the N-by-NRHS solution matrix X.

    DT      (input) COMPLEX array that is the output (the 6th argument)
            of magma_cgeqrf_gpu of size
            2*MIN(M, N)*NB + ((N+31)/32*32 )* MAX(NB, NRHS). 
            The array starts with a block of size MIN(M,N)*NB that stores 
            the triangular T matrices used in the QR factorization, 
            followed by MIN(M,N)*NB block storing the diagonal block 
            inverses for the R matrix, followed by work space of size 
            ((N+31)/32*32 )* MAX(NB, NRHS).

    LDDB    (input) INTEGER
            The leading dimension of the array DB. LDDB >= M.

    HWORK   (workspace/output) COMPLEX array, dimension (LWORK)
            On exit, if INFO = 0, WORK(1) returns the optimal LWORK.

    LWORK   (input) INTEGER
            The dimension of the array WORK, LWORK >= max(1,NRHS).
            For optimum performance LWORK >= (M-N+NB+2*NRHS)*NB, where NB is
            the blocksize given by magma_get_cgeqrf_nb( M ).

            If LWORK = -1, then a workspace query is assumed; the routine
            only calculates the optimal size of the HWORK array, returns
            this value as the first entry of the WORK array.

    INFO    (output) INTEGER
            = 0:  successful exit
            < 0:  if INFO = -i, the i-th argument had an illegal value
    =====================================================================    */

   #define a_ref(a_1,a_2) (dA+(a_2)*(ldda) + (a_1))
   #define d_ref(a_1)     (dT+(lddwork+(a_1))*nb)

    cuFloatComplex c_zero    = MAGMA_C_ZERO;
    cuFloatComplex c_one     = MAGMA_C_ONE;
    cuFloatComplex c_neg_one = MAGMA_C_NEG_ONE;
    cuFloatComplex *dwork;
    magma_int_t i, k, lddwork, rows, ib, ret;

    /* Function Body */
    magma_int_t nb     = magma_get_cgeqrf_nb(m);
    magma_int_t lwkopt = (m-n+nb+2*(nrhs)) * nb;
    long int lquery = (lwork == -1);

    hwork[0] = MAGMA_C_MAKE( (float)lwkopt, 0. );

    *info = 0;
    if (m < 0)
        *info = -1;
    else if (n < 0 || m < n)
        *info = -2;
    else if (nrhs < 0)
        *info = -3;
    else if (ldda < max(1,m))
        *info = -5;
    else if (lddb < max(1,m))
        *info = -8;
    else if (lwork < lwkopt && ! lquery)
        *info = -10;

    if (*info != 0)
        return MAGMA_ERR_ILLEGAL_VALUE;
    else if (lquery)
        return MAGMA_SUCCESS;

    k = min(m,n);
    if (k == 0) {
        hwork[0] = c_one;
        return MAGMA_SUCCESS;
    }

    ret = magma_cunmqr_gpu( MagmaLeft, MagmaConjTrans, 
			    m, nrhs, n,
			    a_ref(0,0), ldda, tau, 
			    dB, lddb, hwork, lwork, dT, nb, info);
    if ( (ret != MAGMA_SUCCESS) || ( *info != 0 ) ) {
	return ret;
    }

    lddwork= k;
    dwork = dT+2*lddwork*nb;

    i    = (k-1)/nb * nb;
    ib   = n-i;
    rows = m-i;

    blasf77_ctrsm( MagmaLeftStr, MagmaUpperStr, MagmaNoTransStr, MagmaNonUnitStr, 
                   &ib, &nrhs, 
                   &c_one, hwork,         &rows, 
                           hwork+rows*ib, &rows);

    // update the solution vector
    cublasSetMatrix(rows, nrhs, sizeof(cuFloatComplex),
                    hwork+rows*ib, rows, dwork+i, lddb);

    // update c
    if (nrhs == 1)
        cublasCgemv( MagmaNoTrans, i, ib, 
                     c_neg_one, a_ref(0, i), ldda,
                                dwork + i,   1, 
                     c_one,     dB,           1);
    else
        cublasCgemm( MagmaNoTrans, MagmaNoTrans, 
                     i, nrhs, ib, 
                     c_neg_one, a_ref(0, i), ldda,
                                dwork + i,   lddb, 
                     c_one,     dB,           lddb);

    int start = i-nb;
    if (nb < k) {
        for (i = start; i >=0; i -= nb) {
            ib = min(k-i, nb);
            rows = m -i;

            if (i + ib < n) {
                if (nrhs == 1)
                    {
                        cublasCgemv( MagmaNoTrans, ib, ib, 
                                     c_one,  d_ref(i), ib,
                                             dB+i,      1, 
                                     c_zero, dwork+i,  1);
                        cublasCgemv( MagmaNoTrans, i, ib, 
                                     c_neg_one, a_ref(0, i), ldda,
                                                dwork + i,   1, 
                                     c_one,     dB,           1);
                    }
                else
                    {
                        cublasCgemm( MagmaNoTrans, MagmaNoTrans, 
                                     ib, nrhs, ib, 
                                     c_one,  d_ref(i), ib,
                                             dB+i,      lddb, 
                                     c_zero, dwork+i,  lddb);
                        cublasCgemm( MagmaNoTrans, MagmaNoTrans, 
                                     i, nrhs, ib, 
                                     c_neg_one, a_ref(0, i), ldda,
                                                dwork + i,   lddb, 
                                     c_one,     dB,          lddb);
                    }
            }
        }
    }

    cudaMemcpy2D(dB,    lddb*sizeof(cuFloatComplex),
		 dwork, lddb*sizeof(cuFloatComplex),
		 (n)*sizeof(cuFloatComplex), nrhs, cudaMemcpyDeviceToDevice);
    
    return MAGMA_SUCCESS;
}

#undef a_ref
#undef d_ref