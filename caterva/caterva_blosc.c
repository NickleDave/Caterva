/*
 * Copyright (C) 2018 Francesc Alted, Aleix Alcacer.
 * Copyright (C) 2019-present Blosc Development team <blosc@blosc.org>
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

#include <caterva.h>
#include "assert.h"


// big <-> little-endian and store it in a memory position.  Sizes supported: 1, 2, 4, 8 bytes.
static void swap_store(void *dest, const void *pa, int size) {
    uint8_t* pa_ = (uint8_t*)pa;
    uint8_t* pa2_ = malloc((size_t)size);
    int i = 1;                    /* for big/little endian detection */
    char* p = (char*)&i;

    if (p[0] == 1) {
        /* little endian */
        switch (size) {
            case 8:
                pa2_[0] = pa_[7];
                pa2_[1] = pa_[6];
                pa2_[2] = pa_[5];
                pa2_[3] = pa_[4];
                pa2_[4] = pa_[3];
                pa2_[5] = pa_[2];
                pa2_[6] = pa_[1];
                pa2_[7] = pa_[0];
                break;
            case 4:
                pa2_[0] = pa_[3];
                pa2_[1] = pa_[2];
                pa2_[2] = pa_[1];
                pa2_[3] = pa_[0];
                break;
            case 2:
                pa2_[0] = pa_[1];
                pa2_[1] = pa_[0];
                break;
            case 1:
                pa2_[0] = pa_[0];
                break;
            default:
                DEBUG_PRINT("Unhandled size");
        }
    }
    memcpy(dest, pa2_, size);
    free(pa2_);
}


static int32_t serialize_meta(int8_t ndim, int64_t *shape, const int32_t *pshape, uint8_t **smeta) {
    // Allocate space for Caterva metalayer
    int32_t max_smeta_len = 1 + 1 + 1 + (1 + ndim * (1 + sizeof(int64_t))) + \
        (1 + ndim * (1 + sizeof(int32_t))) + (1 + ndim * (1 + sizeof(int32_t)));
    *smeta = malloc((size_t)max_smeta_len);
    uint8_t *pmeta = *smeta;

    // Build an array with 5 entries (version, ndim, shape, pshape, bshape)
    *pmeta++ = 0x90 + 5;

    // version entry
    *pmeta++ = CATERVA_METALAYER_VERSION;  // positive fixnum (7-bit positive integer)
    assert(pmeta - *smeta < max_smeta_len);

    // ndim entry
    *pmeta++ = (uint8_t)ndim;  // positive fixnum (7-bit positive integer)
    assert(pmeta - *smeta < max_smeta_len);

    // shape entry
    *pmeta++ = (uint8_t)(0x90) + ndim;  // fix array with ndim elements
    for (int8_t i = 0; i < ndim; i++) {
        *pmeta++ = 0xd3;  // int64
        swap_store(pmeta, shape + i, sizeof(int64_t));
        pmeta += sizeof(int64_t);
    }
    assert(pmeta - *smeta < max_smeta_len);

    // pshape entry
    *pmeta++ = (uint8_t)(0x90) + ndim;  // fix array with ndim elements
    for (int8_t i = 0; i < ndim; i++) {
        *pmeta++ = 0xd2;  // int32
        swap_store(pmeta, pshape + i, sizeof(int32_t));
        pmeta += sizeof(int32_t);
    }
    assert(pmeta - *smeta <= max_smeta_len);

    // bshape entry
    *pmeta++ = (uint8_t)(0x90) + ndim;  // fix array with ndim elements
    int32_t *bshape = malloc(CATERVA_MAXDIM * sizeof(int32_t));
    for (int8_t i = 0; i < ndim; i++) {
        *pmeta++ = 0xd2;  // int32
        bshape[i] = 0;  // FIXME: update when support for multidimensional bshapes would be ready
        // NOTE: we need to initialize the header so as to avoid false negatives in valgrind
        swap_store(pmeta, bshape + i, sizeof(int32_t));
        pmeta += sizeof(int32_t);
    }
    free(bshape);
    assert(pmeta - *smeta <= max_smeta_len);
    int32_t slen = (int32_t)(pmeta - *smeta);

    return slen;
}


static int32_t deserialize_meta(uint8_t *smeta, uint32_t smeta_len, caterva_dims_t *shape, caterva_dims_t *pshape) {
    uint8_t *pmeta = smeta;

    // Check that we have an array with 5 entries (version, ndim, shape, pshape, bshape)
    assert(*pmeta == 0x90 + 5);
    pmeta += 1;
    assert(pmeta - smeta < smeta_len);

    // version entry
    int8_t version = pmeta[0];  // positive fixnum (7-bit positive integer)
    assert (version <= CATERVA_METALAYER_VERSION);
    pmeta += 1;
    assert(pmeta - smeta < smeta_len);

    // ndim entry
    int8_t ndim = pmeta[0];  // positive fixnum (7-bit positive integer)
    assert (ndim <= CATERVA_MAXDIM);
    pmeta += 1;
    assert(pmeta - smeta < smeta_len);
    shape->ndim = ndim;
    pshape->ndim = ndim;

    // shape entry
    // Initialize to ones, as required by Caterva
    for (int i = 0; i < CATERVA_MAXDIM; i++) shape->dims[i] = 1;
    assert(*pmeta == (uint8_t)(0x90) + ndim);  // fix array with ndim elements
    pmeta += 1;
    for (int8_t i = 0; i < ndim; i++) {
        assert(*pmeta == 0xd3);   // int64
        pmeta += 1;
        swap_store(shape->dims + i, pmeta, sizeof(int64_t));
        pmeta += sizeof(int64_t);
    }
    assert(pmeta - smeta < smeta_len);

    // pshape entry
    // Initialize to ones, as required by Caterva
    for (int i = 0; i < CATERVA_MAXDIM; i++) pshape->dims[i] = 1;
    assert(*pmeta == (uint8_t)(0x90) + ndim);  // fix array with ndim elements
    pmeta += 1;
    for (int8_t i = 0; i < ndim; i++) {
        assert(*pmeta == 0xd2);  // int32
        pmeta += 1;
        swap_store(pshape->dims + i, pmeta, sizeof(int32_t));
        pmeta += sizeof(int32_t);
    }
    assert(pmeta - smeta <= smeta_len);

    // bshape entry
    // Initialize to ones, as required by Caterva
    // for (int i = 0; i < CATERVA_MAXDIM; i++) bshape->dims[i] = 1;
    assert(*pmeta == (uint8_t)(0x90) + ndim);  // fix array with ndim elements
    pmeta += 1;
    for (int8_t i = 0; i < ndim; i++) {
        assert(*pmeta == 0xd2);  // int32
        pmeta += 1;
        // swap_store(bshape->dims + i, pmeta, sizeof(int32_t));
        pmeta += sizeof(int32_t);
    }
    assert(pmeta - smeta <= smeta_len);
    uint32_t slen = (uint32_t)(pmeta - smeta);
    assert(slen == smeta_len);
    return 0;
}


caterva_array_t *caterva_blosc_from_frame(caterva_ctx_t *ctx, blosc2_frame *frame, bool copy) {
    /* Create a caterva_array_t buffer */
    caterva_array_t *carr = (caterva_array_t *) ctx->alloc(sizeof(caterva_array_t));
    if (carr == NULL) {
        return NULL;
    }
    /* Copy context to caterva_array_t */
    carr->ctx = (caterva_ctx_t *) ctx->alloc(sizeof(caterva_ctx_t));
    memcpy(&carr->ctx[0], &ctx[0], sizeof(caterva_ctx_t));

    /* Create a schunk out of the frame */
    blosc2_schunk *sc = blosc2_schunk_from_frame(frame, copy);
    carr->sc = sc;
    carr->storage = CATERVA_STORAGE_BLOSC;

    blosc2_dparams *dparams;
    blosc2_schunk_get_dparams(carr->sc, &dparams);
    blosc2_cparams *cparams;
    blosc2_schunk_get_cparams(carr->sc, &cparams);
    memcpy(&carr->ctx->dparams, dparams, sizeof(blosc2_dparams));
    memcpy(&carr->ctx->cparams, cparams, sizeof(blosc2_cparams));

    // Deserialize the caterva metalayer
    caterva_dims_t shape;
    caterva_dims_t pshape;
    uint8_t *smeta;
    uint32_t smeta_len;
    blosc2_get_metalayer(sc, "caterva", &smeta, &smeta_len);
    deserialize_meta(smeta, smeta_len, &shape, &pshape);
    carr->size = 1;
    carr->psize = 1;
    carr->esize = 1;
    carr->ndim = pshape.ndim;

    for (int i = 0; i < CATERVA_MAXDIM; i++) {
        carr->shape[i] = shape.dims[i];
        carr->size *= shape.dims[i];
        carr->pshape[i] = (int32_t)(pshape.dims[i]);
        carr->psize *= carr->pshape[i];
        if (shape.dims[i] % pshape.dims[i] == 0) {
            // The case for shape.dims[i] == 1 and pshape.dims[i] == 1 is handled here
            carr->eshape[i] = shape.dims[i];
        } else {
            carr->eshape[i] = shape.dims[i] + pshape.dims[i] - shape.dims[i] % pshape.dims[i];
        }
        carr->esize *= carr->eshape[i];
    }

    // The partition cache (empty initially)
    carr->part_cache.data = NULL;
    carr->part_cache.nchunk = -1;  // means no valid cache yet
    carr->empty = false;
    if (carr->sc->nchunks == carr->esize / carr->psize) {
        carr->filled = true;
    } else {
        carr->filled = false;
    }

    return carr;
}


caterva_array_t *caterva_blosc_from_sframe(caterva_ctx_t *ctx, uint8_t *sframe, int64_t len, bool copy) {
    // Generate a real frame first
    blosc2_frame *frame = blosc2_frame_from_sframe(sframe, len, copy);
    // ...and create a caterva array out of it
    caterva_array_t *array = caterva_from_frame(ctx, frame, copy);
    if (copy) {
        // We don't need the frame anymore
        blosc2_free_frame(frame);
    }
    return array;
}


caterva_array_t *caterva_blosc_from_file(caterva_ctx_t *ctx, const char *filename, bool copy) {
    // Open the frame on-disk...
    blosc2_frame *frame = blosc2_frame_from_file(filename);
    // ...and create a caterva array out of it
    caterva_array_t *array = caterva_from_frame(ctx, frame, copy);
    if (copy) {
        // We don't need the frame anymore
        blosc2_free_frame(frame);
    }
    return array;
}


int caterva_blosc_free_array(caterva_array_t *carr) {
    if (carr->sc != NULL) {
        blosc2_free_schunk(carr->sc);
    }
    return 0;
}


int caterva_blosc_append(caterva_array_t *carr, void *part, int64_t partsize) {
    blosc2_schunk_append_buffer(carr->sc, part, partsize);
    return 0;
}


int caterva_blosc_from_buffer(caterva_array_t *dest, caterva_dims_t *shape, const void *src) {
    const int8_t *src_b = (int8_t *) src;

    if (dest->sc->nbytes > 0) {
        printf("Caterva container must be empty!");
        return -1;
    }
    int64_t d_shape[CATERVA_MAXDIM];
    int64_t d_eshape[CATERVA_MAXDIM];
    int32_t d_pshape[CATERVA_MAXDIM];
    int8_t d_ndim = dest->ndim;

    for (int i = 0; i < CATERVA_MAXDIM; ++i) {
        d_shape[(CATERVA_MAXDIM - d_ndim + i) % CATERVA_MAXDIM] = dest->shape[i];
        d_eshape[(CATERVA_MAXDIM - d_ndim + i) % CATERVA_MAXDIM] = dest->eshape[i];
        d_pshape[(CATERVA_MAXDIM - d_ndim + i) % CATERVA_MAXDIM] = dest->pshape[i];
    }

    caterva_ctx_t *ctx = dest->ctx;
    int32_t typesize = dest->sc->typesize;
    int8_t *chunk = ctx->alloc((size_t) dest->psize * typesize);

    /* Calculate the constants out of the for  */
    int64_t aux[CATERVA_MAXDIM];
    aux[7] = d_eshape[7] / d_pshape[7];
    for (int i = CATERVA_MAXDIM - 2; i >= 0; i--) {
        aux[i] = d_eshape[i] / d_pshape[i] * aux[i + 1];
    }

    /* Fill each chunk buffer */
    int64_t desp[CATERVA_MAXDIM];
    int32_t actual_psize[CATERVA_MAXDIM];
    for (int64_t ci = 0; ci < dest->esize / dest->psize; ci++) {
        memset(chunk, 0, dest->psize * typesize);
        /* Calculate the coord. of the chunk first element */
        desp[7] = ci % (d_eshape[7] / d_pshape[7]) * d_pshape[7];
        for (int i = CATERVA_MAXDIM - 2; i >= 0; i--) {
            desp[i] = ci % (aux[i]) / (aux[i + 1]) * d_pshape[i];
        }
        /* Calculate if padding with 0s is needed for this chunk */
        for (int i = CATERVA_MAXDIM - 1; i >= 0; i--) {
            if (desp[i] + d_pshape[i] > d_shape[i]) {
                actual_psize[i] = d_shape[i] - desp[i];
            } else {
                actual_psize[i] = d_pshape[i];
            }
        }
        int32_t seq_copylen = actual_psize[7] * typesize;
        /* Copy each line of data from chunk to arr */
        int64_t ii[CATERVA_MAXDIM];
        for (ii[6] = 0; ii[6] < actual_psize[6]; ii[6]++) {
            for (ii[5] = 0; ii[5] < actual_psize[5]; ii[5]++) {
                for (ii[4] = 0; ii[4] < actual_psize[4]; ii[4]++) {
                    for (ii[3] = 0; ii[3] < actual_psize[3]; ii[3]++) {
                        for (ii[2] = 0; ii[2] < actual_psize[2]; ii[2]++) {
                            for (ii[1] = 0; ii[1] < actual_psize[1]; ii[1]++) {
                                for (ii[0] = 0; ii[0] < actual_psize[0]; ii[0]++) {
                                    int64_t d_a = d_pshape[7];
                                    int64_t d_coord_f = 0;
                                    for (int i = CATERVA_MAXDIM - 2; i >= 0; i--) {
                                        d_coord_f += ii[i] * d_a;
                                        d_a *= d_pshape[i];
                                    }
                                    int64_t s_coord_f = desp[7];
                                    int64_t s_a = d_shape[7];
                                    for (int i = CATERVA_MAXDIM - 2; i >= 0; i--) {
                                        s_coord_f += (desp[i] + ii[i]) * s_a;
                                        s_a *= d_shape[i];
                                    }
                                    memcpy(chunk + d_coord_f * typesize, src_b + s_coord_f * typesize, seq_copylen);
                                }
                            }
                        }
                    }
                }
            }
        }
        caterva_append(dest, chunk, (size_t) dest->psize * typesize);
    }
    ctx->free(chunk);

    return 0;
}


int caterva_blosc_to_buffer(caterva_array_t *src, void *dest) {
    int8_t *d_b = (int8_t *) dest;
    int64_t s_shape[CATERVA_MAXDIM];
    int64_t s_pshape[CATERVA_MAXDIM];
    int64_t s_eshape[CATERVA_MAXDIM];
    int8_t s_ndim = src->ndim;

    for (int i = 0; i < CATERVA_MAXDIM; ++i) {
        s_shape[(CATERVA_MAXDIM - s_ndim + i) % CATERVA_MAXDIM] = src->shape[i];
        s_eshape[(CATERVA_MAXDIM - s_ndim + i) % CATERVA_MAXDIM] = src->eshape[i];
        s_pshape[(CATERVA_MAXDIM - s_ndim + i) % CATERVA_MAXDIM] = src->pshape[i];
    }

    /* Initialise a chunk buffer */
    caterva_ctx_t *ctx = src->ctx;
    int typesize = src->sc->typesize;
    int8_t *chunk = (int8_t *) ctx->alloc((size_t) src->psize * typesize);

    /* Calculate the constants out of the for  */
    int64_t aux[CATERVA_MAXDIM];
    aux[7] = s_eshape[7] / s_pshape[7];
    for (int i = CATERVA_MAXDIM - 2; i >= 0; i--) {
        aux[i] = s_eshape[i] / s_pshape[i] * aux[i + 1];
    }

    /* Fill array from schunk (chunk by chunk) */
    int64_t desp[CATERVA_MAXDIM], r[CATERVA_MAXDIM];
    for (int64_t ci = 0; ci < src->esize / src->psize; ci++) {
        blosc2_schunk_decompress_chunk(src->sc, (int) ci, chunk, (size_t) src->psize * typesize);
        /* Calculate the coord. of the chunk first element in arr buffer */
        desp[7] = ci % aux[7] * s_pshape[7];
        for (int i = CATERVA_MAXDIM - 2; i >= 0; i--) {
            desp[i] = ci % (aux[i]) / (aux[i + 1]) * s_pshape[i];
        }
        /* Calculate if pad with 0 are needed in this chunk */
        for (int i = CATERVA_MAXDIM - 1; i >= 0; i--) {
            if (desp[i] + s_pshape[i] > s_shape[i]) {
                r[i] = s_shape[i] - desp[i];
            } else {
                r[i] = s_pshape[i];
            }
        }

        /* Copy each line of data from chunk to arr */
        int64_t s_coord_f, d_coord_f, s_a, d_a;
        int64_t ii[CATERVA_MAXDIM];
        for (ii[6] = 0; ii[6] < r[6]; ii[6]++) {
            for (ii[5] = 0; ii[5] < r[5]; ii[5]++) {
                for (ii[4] = 0; ii[4] < r[4]; ii[4]++) {
                    for (ii[3] = 0; ii[3] < r[3]; ii[3]++) {
                        for (ii[2] = 0; ii[2] < r[2]; ii[2]++) {
                            for (ii[1] = 0; ii[1] < r[1]; ii[1]++) {
                                for (ii[0] = 0; ii[0] < r[0]; ii[0]++) {
                                    s_coord_f = 0;
                                    s_a = s_pshape[7];
                                    for (int i = CATERVA_MAXDIM - 2; i >= 0; i--) {
                                        s_coord_f += ii[i] * s_a;
                                        s_a *= s_pshape[i];
                                    }
                                    d_coord_f = desp[7];
                                    d_a = s_shape[7];
                                    for (int i = CATERVA_MAXDIM - 2; i >= 0; i--) {
                                        d_coord_f += (desp[i] + ii[i]) * d_a;
                                        d_a *= s_shape[i];
                                    }
                                    memcpy(&d_b[d_coord_f * typesize], &chunk[s_coord_f * typesize],
                                           r[7] * typesize);
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    ctx->free(chunk);
    return 0;
}


int caterva_blosc_get_slice_buffer(void *dest, caterva_array_t *src, caterva_dims_t *start,
                                   caterva_dims_t *stop, caterva_dims_t *d_pshape) {
    uint8_t *bdest = dest;   // for allowing pointer arithmetic
    int64_t start_[CATERVA_MAXDIM];
    int64_t stop_[CATERVA_MAXDIM];
    int64_t d_pshape_[CATERVA_MAXDIM];
    int64_t s_pshape[CATERVA_MAXDIM];
    int64_t s_eshape[CATERVA_MAXDIM];
    int8_t s_ndim = src->ndim;

    for (int i = 0; i < CATERVA_MAXDIM; ++i) {
        start_[(CATERVA_MAXDIM - s_ndim + i) % CATERVA_MAXDIM] = start->dims[i];
        stop_[(CATERVA_MAXDIM - s_ndim + i) % CATERVA_MAXDIM] = stop->dims[i];
        d_pshape_[(CATERVA_MAXDIM - s_ndim + i) % CATERVA_MAXDIM] = d_pshape->dims[i];
        s_eshape[(CATERVA_MAXDIM - s_ndim + i) % CATERVA_MAXDIM] = src->eshape[i];
        s_pshape[(CATERVA_MAXDIM - s_ndim + i) % CATERVA_MAXDIM] = src->pshape[i];
    }

    // Acceleration path for the case where we are doing (1-dim) aligned chunk reads
    if ((s_ndim == 1) && (src->pshape[0] == d_pshape->dims[0]) &&
        (start->dims[0] % src->pshape[0] == 0) && (stop->dims[0] % src->pshape[0] == 0)) {
        int nchunk = (int)(start->dims[0] / src->pshape[0]);
        // In case of an aligned read, decompress directly in destination
        blosc2_schunk_decompress_chunk(src->sc, nchunk, bdest, (size_t)src->psize * src->sc->typesize);
        return 0;
    }

    for (int j = 0; j < CATERVA_MAXDIM - s_ndim; ++j) {
        start_[j] = 0;
    }
    /* Create chunk buffers */
    caterva_ctx_t *ctx = src->ctx;
    int typesize = src->sc->typesize;

    uint8_t *chunk;
    bool local_cache;
    if (src->part_cache.data == NULL) {
        chunk = (uint8_t *) ctx->alloc((size_t) src->psize * typesize);
        local_cache = true;
    } else {
        chunk = src->part_cache.data;
        local_cache = false;
    }
    int64_t i_start[8], i_stop[8];
    for (int i = 0; i < CATERVA_MAXDIM; ++i) {
        i_start[i] = start_[i] / s_pshape[i];
        i_stop[i] = (stop_[i] - 1) / s_pshape[i];
    }

    /* Calculate the used chunks */
    int64_t ii[CATERVA_MAXDIM], jj[CATERVA_MAXDIM];
    int64_t c_start[CATERVA_MAXDIM], c_stop[CATERVA_MAXDIM];
    for (ii[0] = i_start[0]; ii[0] <= i_stop[0]; ++ii[0]) {
        for (ii[1] = i_start[1]; ii[1] <= i_stop[1]; ++ii[1]) {
            for (ii[2] = i_start[2]; ii[2] <= i_stop[2]; ++ii[2]) {
                for (ii[3] = i_start[3]; ii[3] <= i_stop[3]; ++ii[3]) {
                    for (ii[4] = i_start[4]; ii[4] <= i_stop[4]; ++ii[4]) {
                        for (ii[5] = i_start[5]; ii[5] <= i_stop[5]; ++ii[5]) {
                            for (ii[6] = i_start[6]; ii[6] <= i_stop[6]; ++ii[6]) {
                                for (ii[7] = i_start[7]; ii[7] <= i_stop[7]; ++ii[7]) {
                                    int nchunk = 0;
                                    int inc = 1;
                                    for (int i = CATERVA_MAXDIM - 1; i >= 0; --i) {
                                        nchunk += (int) (ii[i] * inc);
                                        inc *= (int) (s_eshape[i] / s_pshape[i]);
                                    }

                                    if ((src->part_cache.data == NULL) || (src->part_cache.nchunk != nchunk)) {
                                        blosc2_schunk_decompress_chunk(src->sc, nchunk, chunk,
                                                                       (size_t) src->psize * typesize);
                                    }
                                    if (src->part_cache.data != NULL) {
                                        src->part_cache.nchunk = nchunk;
                                    }

                                    for (int i = 0; i < CATERVA_MAXDIM; ++i) {
                                        if (ii[i] == (start_[i] / s_pshape[i])) {
                                            c_start[i] = start_[i] % s_pshape[i];
                                        } else {
                                            c_start[i] = 0;
                                        }
                                        if (ii[i] == stop_[i] / s_pshape[i]) {
                                            c_stop[i] = stop_[i] % s_pshape[i];
                                        } else {
                                            c_stop[i] = s_pshape[i];
                                        }
                                    }
                                    jj[7] = c_start[7];
                                    for (jj[0] = c_start[0]; jj[0] < c_stop[0]; ++jj[0]) {
                                        for (jj[1] = c_start[1]; jj[1] < c_stop[1]; ++jj[1]) {
                                            for (jj[2] = c_start[2]; jj[2] < c_stop[2]; ++jj[2]) {
                                                for (jj[3] = c_start[3]; jj[3] < c_stop[3]; ++jj[3]) {
                                                    for (jj[4] = c_start[4]; jj[4] < c_stop[4]; ++jj[4]) {
                                                        for (jj[5] = c_start[5]; jj[5] < c_stop[5]; ++jj[5]) {
                                                            for (jj[6] = c_start[6]; jj[6] < c_stop[6]; ++jj[6]) {
                                                                int64_t chunk_pointer = 0;
                                                                int64_t chunk_pointer_inc = 1;
                                                                for (int i = CATERVA_MAXDIM - 1; i >= 0; --i) {
                                                                    chunk_pointer += jj[i] * chunk_pointer_inc;
                                                                    chunk_pointer_inc *= s_pshape[i];
                                                                }
                                                                int64_t buf_pointer = 0;
                                                                int64_t buf_pointer_inc = 1;
                                                                for (int i = CATERVA_MAXDIM - 1; i >= 0; --i) {
                                                                    buf_pointer += (jj[i] + s_pshape[i] * ii[i] -
                                                                        start_[i]) * buf_pointer_inc;
                                                                    buf_pointer_inc *= d_pshape_[i];
                                                                }
                                                                memcpy(&bdest[buf_pointer * typesize],
                                                                       &chunk[chunk_pointer * typesize],
                                                                       (c_stop[7] - c_start[7]) * typesize);
                                                            }
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    if (local_cache) {
        ctx->free(chunk);
    }

    return 0;
}


int caterva_blosc_get_slice(caterva_array_t *dest, caterva_array_t *src, caterva_dims_t *start,
                            caterva_dims_t *stop) {

    caterva_ctx_t *ctx = src->ctx;
    int typesize = ctx->cparams.typesize;

    uint8_t *chunk = ctx->alloc((size_t) dest->psize * typesize);
    int64_t d_pshape[CATERVA_MAXDIM];
    int64_t d_start[CATERVA_MAXDIM];
    int64_t d_stop[CATERVA_MAXDIM];
    int8_t d_ndim = dest->ndim;
    for (int i = 0; i < CATERVA_MAXDIM; ++i) {
        d_pshape[(CATERVA_MAXDIM - d_ndim + i) % CATERVA_MAXDIM] = dest->pshape[i];
        d_start[(CATERVA_MAXDIM - d_ndim + i) % CATERVA_MAXDIM] = start->dims[i];
        d_stop[(CATERVA_MAXDIM - d_ndim + i) % CATERVA_MAXDIM] = stop->dims[i];
    }
    int64_t ii[CATERVA_MAXDIM];
    for (ii[0] = d_start[0]; ii[0] < d_stop[0]; ii[0] += d_pshape[0]) {
        for (ii[1] = d_start[1]; ii[1] < d_stop[1]; ii[1] += d_pshape[1]) {
            for (ii[2] = d_start[2]; ii[2] < d_stop[2]; ii[2] += d_pshape[2]) {
                for (ii[3] = d_start[3]; ii[3] < d_stop[3]; ii[3] += d_pshape[3]) {
                    for (ii[4] = d_start[4]; ii[4] < d_stop[4]; ii[4] += d_pshape[4]) {
                        for (ii[5] = d_start[5]; ii[5] < d_stop[5]; ii[5] += d_pshape[5]) {
                            for (ii[6] = d_start[6]; ii[6] < d_stop[6]; ii[6] += d_pshape[6]) {
                                for (ii[7] = d_start[7]; ii[7] < d_stop[7]; ii[7] += d_pshape[7]) {
                                    memset(chunk, 0, dest->psize * typesize);
                                    int64_t jj[CATERVA_MAXDIM];
                                    for (int i = 0; i < CATERVA_MAXDIM; ++i) {
                                        if (ii[i] + d_pshape[i] > d_stop[i]) {
                                            jj[i] = d_stop[i];
                                        } else {
                                            jj[i] = ii[i] + d_pshape[i];
                                        }
                                    }
                                    int64_t start_[CATERVA_MAXDIM];
                                    int64_t stop_[CATERVA_MAXDIM];
                                    int64_t d_pshape_[CATERVA_MAXDIM];
                                    for (int i = 0; i < CATERVA_MAXDIM; ++i) {
                                        start_[i] = ii[(CATERVA_MAXDIM - d_ndim + i) % CATERVA_MAXDIM];
                                        stop_[i] = jj[(CATERVA_MAXDIM - d_ndim + i) % CATERVA_MAXDIM];
                                        d_pshape_[i] = d_pshape[(CATERVA_MAXDIM - d_ndim + i) % CATERVA_MAXDIM];
                                    }
                                    caterva_dims_t start__ = caterva_new_dims(start_, d_ndim);
                                    caterva_dims_t stop__ = caterva_new_dims(stop_, d_ndim);
                                    caterva_dims_t d_pshape__ = caterva_new_dims(d_pshape_, d_ndim);
                                    caterva_get_slice_buffer(chunk, src, &start__, &stop__, &d_pshape__);
                                    caterva_append(dest, chunk, dest->psize * typesize);
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    free(chunk);

    return 0;
}

int caterva_blosc_squeeze(caterva_array_t *src) {
    uint8_t nones = 0;
    int64_t newshape_[CATERVA_MAXDIM];
    int32_t newpshape_[CATERVA_MAXDIM];

    for (int i = 0; i < src->ndim; ++i) {
        if (src->shape[i] != 1) {
            newshape_[nones] = src->shape[i];
            newpshape_[nones] = src->pshape[i];
            nones += 1;
        }
    }
    for (int i = 0; i < CATERVA_MAXDIM; ++i) {
        if (i < nones) {
            src->pshape[i] = newpshape_[i];
        } else {
            src->pshape[i] = 1;
        }
    }

    src->ndim = nones;
    caterva_dims_t newshape = caterva_new_dims(newshape_, nones);
    caterva_update_shape(src, &newshape);

    return 0;
}


int caterva_blosc_copy(caterva_array_t *dest, caterva_array_t *src) {
    int64_t start_[CATERVA_MAXDIM] = {0, 0, 0, 0, 0, 0, 0, 0};
    caterva_dims_t start = caterva_new_dims(start_, src->ndim);
    int64_t stop_[CATERVA_MAXDIM];
    for (int i = 0; i < src->ndim; ++i) {
        stop_[i] = src->shape[i];
    }
    caterva_dims_t stop = caterva_new_dims(stop_, src->ndim);

    caterva_get_slice(dest, src, &start, &stop);

    return 0;
}


int caterva_blosc_update_shape(caterva_array_t *carr, caterva_dims_t *shape) {
    if (carr->ndim != shape->ndim) {
        printf("caterva array ndim and shape ndim are not equal\n");
        return -1;
    }
    carr->size = 1;
    carr->esize = 1;
    for (int i = 0; i < CATERVA_MAXDIM; ++i) {
        carr->shape[i] = shape->dims[i];
        if (i < shape->ndim) {
            if (shape->dims[i] % carr->pshape[i] == 0) {
                carr->eshape[i] = shape->dims[i];
            } else {
                carr->eshape[i] = shape->dims[i] + carr->pshape[i] - shape->dims[i] % carr->pshape[i];
            }
        } else {
            carr->eshape[i] = 1;
        }
        carr->size *= carr->shape[i];
        carr->esize *= carr->eshape[i];
    }

    uint8_t *smeta = NULL;
    // Serialize the dimension info ...
    int32_t smeta_len = serialize_meta(carr->ndim, carr->shape, carr->pshape, &smeta);
    if (smeta_len < 0) {
        fprintf(stderr, "error during serializing dims info for Caterva");
        return -1;
    }
    // ... and update it in its metalayer
    if (blosc2_has_metalayer(carr->sc, "caterva") < 0) {
        int retcode = blosc2_add_metalayer(carr->sc, "caterva", smeta, (uint32_t) smeta_len);
        if (retcode < 0) {
            return -1;
        }
    }
    else {
        int retcode = blosc2_update_metalayer(carr->sc, "caterva", smeta, (uint32_t) smeta_len);
        if (retcode < 0) {
            return -1;
        }
    }

    return 0;
}


caterva_array_t *caterva_blosc_empty_array(caterva_ctx_t *ctx, blosc2_frame *frame, caterva_dims_t *pshape) {
    /* Create a caterva_array_t buffer */
    caterva_array_t *carr = (caterva_array_t *) ctx->alloc(sizeof(caterva_array_t));
    carr->size = 1;
    carr->psize = 1;
    carr->esize = 1;
    // The partition cache (empty initially)
    carr->part_cache.data = NULL;
    carr->part_cache.nchunk = -1;  // means no valid cache yet
    carr->sc = NULL;
    carr->buf = NULL;

    carr->storage = CATERVA_STORAGE_BLOSC;
    carr->ndim = pshape->ndim;
    for (unsigned int i = 0; i < CATERVA_MAXDIM; i++) {
        carr->pshape[i] = (int32_t)(pshape->dims[i]);
        carr->shape[i] = 1;
        carr->eshape[i] = 1;
        carr->psize *= carr->pshape[i];
    }

    blosc2_schunk *sc = blosc2_new_schunk(ctx->cparams, ctx->dparams, frame);
    if (frame != NULL) {
        // Serialize the dimension info in the associated frame
        if (sc->nmetalayers >= BLOSC2_MAX_METALAYERS) {
            fprintf(stderr, "the number of metalayers for this frame has been exceeded\n");
            return NULL;
        }
        uint8_t *smeta = NULL;
        int32_t smeta_len = serialize_meta(carr->ndim, carr->shape, carr->pshape, &smeta);
        if (smeta_len < 0) {
            fprintf(stderr, "error during serializing dims info for Caterva");
            return NULL;
        }
        // And store it in caterva metalayer
        int retcode = blosc2_add_metalayer(sc, "caterva", smeta, (uint32_t)smeta_len);
        if (retcode < 0) {
            return NULL;
        }
        free(smeta);
    }
    /* Create a schunk (for a frame-disk-backed one, this implies serializing the header on-disk */
    carr->sc = sc;

    return carr;
}