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

#include <assert.h>
#include <caterva.h>

static void index_unidim_to_multidim(int8_t ndim, int64_t *shape, int64_t i, int64_t *index) {
    int64_t strides[CATERVA_MAX_DIM];
    strides[ndim - 1] = 1;
    for (int j = ndim - 2; j >= 0; --j) {
        strides[j] = shape[j + 1] * strides[j + 1];
    }

    index[0] = i / strides[0];
    for (int j = 1; j < ndim; ++j) {
        index[j] = (i % strides[j - 1]) / strides[j];
    }
}

// big <-> little-endian and store it in a memory position.  Sizes supported: 1, 2, 4, 8 bytes.
static void swap_store(void *dest, const void *pa, int size) {
    uint8_t *pa_ = (uint8_t *) pa;
    uint8_t *pa2_ = malloc((size_t) size);
    int i = 1; /* for big/little endian detection */
    char *p = (char *) &i;

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
                fprintf(stderr, "Unhandled nitems: %d\n", size);
        }
    }
    memcpy(dest, pa2_, size);
    free(pa2_);
}

static int32_t serialize_meta(int8_t ndim, int64_t *shape, const int32_t *chunkshape,
                              const int32_t *blockshape, uint8_t **smeta) {
    // Allocate space for Caterva metalayer
    int32_t max_smeta_len = 1 + 1 + 1 + (1 + ndim * (1 + sizeof(int64_t))) +
                            (1 + ndim * (1 + sizeof(int32_t))) + (1 + ndim * (1 + sizeof(int32_t)));
    *smeta = malloc((size_t) max_smeta_len);
    uint8_t *pmeta = *smeta;

    // Build an array with 5 entries (version, ndim, shape, chunkshape, blockshape)
    *pmeta++ = 0x90 + 5;

    // version entry
    *pmeta++ = CATERVA_METALAYER_VERSION;  // positive fixnum (7-bit positive integer)
    assert(pmeta - *smeta < max_smeta_len);

    // ndim entry
    *pmeta++ = (uint8_t) ndim;  // positive fixnum (7-bit positive integer)
    assert(pmeta - *smeta < max_smeta_len);

    // shape entry
    *pmeta++ = (uint8_t)(0x90) + ndim;  // fix array with ndim elements
    for (int8_t i = 0; i < ndim; i++) {
        *pmeta++ = 0xd3;  // int64
        swap_store(pmeta, shape + i, sizeof(int64_t));
        pmeta += sizeof(int64_t);
    }
    assert(pmeta - *smeta < max_smeta_len);

    // chunkshape entry
    *pmeta++ = (uint8_t)(0x90) + ndim;  // fix array with ndim elements
    for (int8_t i = 0; i < ndim; i++) {
        *pmeta++ = 0xd2;  // int32
        swap_store(pmeta, chunkshape + i, sizeof(int32_t));
        pmeta += sizeof(int32_t);
    }
    assert(pmeta - *smeta <= max_smeta_len);

    // blockshape entry
    *pmeta++ = (uint8_t)(0x90) + ndim;  // fix array with ndim elements
    for (int8_t i = 0; i < ndim; i++) {
        *pmeta++ = 0xd2;  // int32
        swap_store(pmeta, blockshape + i, sizeof(int32_t));
        pmeta += sizeof(int32_t);
    }
    assert(pmeta - *smeta <= max_smeta_len);
    int32_t slen = (int32_t)(pmeta - *smeta);

    return slen;
}

static int32_t deserialize_meta(uint8_t *smeta, uint32_t smeta_len, int8_t *ndim, int64_t *shape,
                                int32_t *chunkshape, int32_t *blockshape) {
    uint8_t *pmeta = smeta;
    CATERVA_UNUSED_PARAM(smeta_len);

    // Check that we have an array with 5 entries (version, ndim, shape, chunkshape, blockshape)
    assert(*pmeta == 0x90 + 5);
    pmeta += 1;
    assert((uint32_t)(pmeta - smeta) < smeta_len);

    // version entry
    int8_t version = pmeta[0];  // positive fixnum (7-bit positive integer)
    CATERVA_UNUSED_PARAM(version);
    assert(version <= CATERVA_METALAYER_VERSION);
    pmeta += 1;
    assert((uint32_t)(pmeta - smeta) < smeta_len);

    // ndim entry
    *ndim = pmeta[0];
    int8_t ndim_aux = *ndim;  // positive fixnum (7-bit positive integer)
    assert(ndim_aux <= CATERVA_MAX_DIM);
    pmeta += 1;
    assert((uint32_t)(pmeta - smeta) < smeta_len);

    // shape entry
    // Initialize to ones, as required by Caterva
    for (int i = 0; i < CATERVA_MAX_DIM; i++) shape[i] = 1;
    assert(*pmeta == (uint8_t)(0x90) + ndim_aux);  // fix array with ndim elements
    pmeta += 1;
    for (int8_t i = 0; i < ndim_aux; i++) {
        assert(*pmeta == 0xd3);  // int64
        pmeta += 1;
        swap_store(shape + i, pmeta, sizeof(int64_t));
        pmeta += sizeof(int64_t);
    }
    assert((uint32_t)(pmeta - smeta) < smeta_len);

    // chunkshape entry
    // Initialize to ones, as required by Caterva
    for (int i = 0; i < CATERVA_MAX_DIM; i++) chunkshape[i] = 1;
    assert(*pmeta == (uint8_t)(0x90) + ndim_aux);  // fix array with ndim elements
    pmeta += 1;
    for (int8_t i = 0; i < ndim_aux; i++) {
        assert(*pmeta == 0xd2);  // int32
        pmeta += 1;
        swap_store(chunkshape + i, pmeta, sizeof(int32_t));
        pmeta += sizeof(int32_t);
    }
    assert((uint32_t)(pmeta - smeta) <= smeta_len);

    // blockshape entry
    // Initialize to ones, as required by Caterva
    for (int i = 0; i < CATERVA_MAX_DIM; i++) blockshape[i] = 1;
    assert(*pmeta == (uint8_t)(0x90) + ndim_aux);  // fix array with ndim elements
    pmeta += 1;
    for (int8_t i = 0; i < ndim_aux; i++) {
        assert(*pmeta == 0xd2);  // int32
        pmeta += 1;
        swap_store(blockshape + i, pmeta, sizeof(int32_t));
        pmeta += sizeof(int32_t);
    }
    assert((uint32_t)(pmeta - smeta) <= smeta_len);
    uint32_t slen = (uint32_t)(pmeta - smeta);
    CATERVA_UNUSED_PARAM(slen);
    assert(slen == smeta_len);
    return 0;
}

int
caterva_blosc_from_schunk(caterva_ctx_t *ctx, blosc2_schunk *schunk, caterva_array_t **array) {
    if (ctx == NULL) {
        DEBUG_PRINT("Context is null");
        return CATERVA_ERR_NULL_POINTER;
    }
    if (schunk == NULL) {
        DEBUG_PRINT("Schunk is null");
        return CATERVA_ERR_NULL_POINTER;
    }
    /* Create a caterva_array_t buffer */
    *array = (caterva_array_t *) ctx->cfg->alloc(sizeof(caterva_array_t));

    if (schunk == NULL) {
        DEBUG_PRINT("Schunk is null");
        return CATERVA_ERR_NULL_POINTER;
    }
    (*array)->sc = schunk;
    (*array)->storage = CATERVA_STORAGE_BLOSC;

    blosc2_cparams *cparams;
    if (blosc2_schunk_get_cparams(schunk, &cparams) < 0) {
        DEBUG_PRINT("Blosc error");
        return CATERVA_ERR_NULL_POINTER;
    }

    (*array)->itemsize = (int8_t) cparams->typesize;

    free(cparams);

    // Deserialize the caterva metalayer
    uint8_t *smeta;
    uint32_t smeta_len;
    if (blosc2_meta_get(schunk, "caterva", &smeta, &smeta_len) < 0) {
        DEBUG_PRINT("Blosc error");
        return CATERVA_ERR_BLOSC_FAILED;
    }
    deserialize_meta(smeta, smeta_len, &(*array)->ndim, (*array)->shape, (*array)->chunkshape,
                     (*array)->blockshape);
    free(smeta);

    int64_t *shape = (*array)->shape;
    int32_t *chunkshape = (*array)->chunkshape;
    int32_t *blockshape = (*array)->blockshape;

    (*array)->nitems = 1;
    (*array)->chunknitems = 1;
    (*array)->blocknitems = 1;
    (*array)->extnitems = 1;
    (*array)->extchunknitems = 1;

    for (int i = 0; i < (*array)->ndim; ++i) {
        if (shape[i] != 0) {
            if (shape[i] % chunkshape[i] == 0) {
                (*array)->extshape[i] = shape[i];
            } else {
                (*array)->extshape[i] = shape[i] + chunkshape[i] - shape[i] % chunkshape[i];
            }
            if (chunkshape[i] % blockshape[i] == 0) {
                (*array)->extchunkshape[i] = chunkshape[i];
            } else {
                (*array)->extchunkshape[i] =
                        chunkshape[i] + blockshape[i] - chunkshape[i] % blockshape[i];
            }
        } else {
            (*array)->extshape[i] = 0;
            (*array)->extchunkshape[i] = 0;
        }
        (*array)->nitems *= shape[i];
        (*array)->chunknitems *= chunkshape[i];
        (*array)->blocknitems *= blockshape[i];
        (*array)->extnitems *= (*array)->extshape[i];
        (*array)->extchunknitems *= (*array)->extchunkshape[i];
    }

    for (int i = (*array)->ndim; i < CATERVA_MAX_DIM; ++i) {
        (*array)->shape[i] = 1;
        (*array)->chunkshape[i] = 1;
        (*array)->blockshape[i] = 1;
        (*array)->extshape[i] = 1;
        (*array)->extchunkshape[i] = 1;
    }

    // The partition cache (empty initially)
    (*array)->chunk_cache.data = NULL;
    (*array)->chunk_cache.nchunk = -1;  // means no valid cache yet

    (*array)->buf = NULL;

    if ((*array)->nitems == 0) {
        (*array)->filled = true;
        (*array)->empty = false;
    } else {
        if (schunk->nchunks == (*array)->extnitems / (*array)->chunknitems) {
            (*array)->filled = true;
        } else {
            (*array)->filled = false;
        }
    }

    return CATERVA_SUCCEED;
}

int caterva_blosc_from_serial_schunk(caterva_ctx_t *ctx, uint8_t *serial_schunk, int64_t len,
                                     caterva_array_t **array) {
    blosc2_schunk *sc = blosc2_schunk_from_buffer(serial_schunk, len, true);
    if (sc == NULL) {
        DEBUG_PRINT("Blosc error");
        return CATERVA_ERR_BLOSC_FAILED;
    }
    // ...and create a caterva array out of it
    CATERVA_ERROR(caterva_from_schunk(ctx, sc, array));

    return CATERVA_SUCCEED;
}

int caterva_blosc_open(caterva_ctx_t *ctx, const char *urlpath, caterva_array_t **array) {
    blosc2_schunk *sc = blosc2_schunk_open(urlpath);

    // ...and create a caterva array out of it
    CATERVA_ERROR(caterva_from_schunk(ctx, sc, array));

    return CATERVA_SUCCEED;
}

int caterva_blosc_array_free(caterva_ctx_t *ctx, caterva_array_t **array) {
    CATERVA_UNUSED_PARAM(ctx);

    if ((*array)->sc != NULL) {
        blosc2_schunk_free((*array)->sc);
    }
    return CATERVA_SUCCEED;
}

int caterva_blosc_array_repart_chunk(int8_t *rchunk, int64_t rchunksize, void *chunk,
                                     int64_t chunksize, caterva_array_t *array) {
    if (rchunksize != array->extchunknitems * array->itemsize) {
        CATERVA_ERROR(CATERVA_ERR_INVALID_ARGUMENT);
    }
    if (chunksize != array->chunknitems * array->itemsize) {
        CATERVA_ERROR(CATERVA_ERR_INVALID_ARGUMENT);
    }

    const int8_t *src_b = (int8_t *) chunk;
    memset(rchunk, 0, (size_t) rchunksize);
    int32_t d_pshape[CATERVA_MAX_DIM];
    int64_t d_epshape[CATERVA_MAX_DIM];
    int32_t d_spshape[CATERVA_MAX_DIM];
    int8_t d_ndim = array->ndim;

    for (int i = 0; i < CATERVA_MAX_DIM; ++i) {
        d_pshape[(CATERVA_MAX_DIM - d_ndim + i) % CATERVA_MAX_DIM] = array->chunkshape[i];
        d_epshape[(CATERVA_MAX_DIM - d_ndim + i) % CATERVA_MAX_DIM] = array->extchunkshape[i];
        d_spshape[(CATERVA_MAX_DIM - d_ndim + i) % CATERVA_MAX_DIM] = array->blockshape[i];
    }

    int64_t aux[CATERVA_MAX_DIM];
    aux[7] = d_epshape[7] / d_spshape[7];
    for (int i = CATERVA_MAX_DIM - 2; i >= 0; i--) {
        aux[i] = d_epshape[i] / d_spshape[i] * aux[i + 1];
    }

    /* Fill each block buffer */
    int32_t orig[CATERVA_MAX_DIM];
    int64_t actual_spsize[CATERVA_MAX_DIM];
    for (int32_t sci = 0; sci < array->extchunknitems / array->blocknitems; sci++) {
        /*Calculate the coord. of the block first element */
        orig[7] = sci % (d_epshape[7] / d_spshape[7]) * d_spshape[7];
        for (int i = CATERVA_MAX_DIM - 2; i >= 0; i--) {
            orig[i] = (int32_t)(sci % (aux[i]) / (aux[i + 1]) * d_spshape[i]);
        }
        /* Calculate if padding with 0s is needed for this block */
        for (int i = CATERVA_MAX_DIM - 1; i >= 0; i--) {
            if (orig[i] + d_spshape[i] > d_pshape[i]) {
                actual_spsize[i] = (d_pshape[i] - orig[i]);
            } else {
                actual_spsize[i] = d_spshape[i];
            }
        }
        int32_t seq_copylen = actual_spsize[7] * array->itemsize;
        /* Reorder each line of data from src_b to chunk */
        int64_t ii[CATERVA_MAX_DIM];
        int64_t ncopies = 1;
        for (int i = 0; i < CATERVA_MAX_DIM - 1; ++i) {
            ncopies *= actual_spsize[i];
        }
        for (int ncopy = 0; ncopy < ncopies; ++ncopy) {
            index_unidim_to_multidim(CATERVA_MAX_DIM - 1, actual_spsize, ncopy, ii);

            int64_t d_a = d_spshape[7];
            int64_t d_coord_f = sci * array->blocknitems;
            for (int i = CATERVA_MAX_DIM - 2; i >= 0; i--) {
                d_coord_f += ii[i] * d_a;
                d_a *= d_spshape[i];
            }

            int64_t s_coord_f = orig[7];
            int64_t s_a = d_pshape[7];
            for (int i = CATERVA_MAX_DIM - 2; i >= 0; i--) {
                s_coord_f += (orig[i] + ii[i]) * s_a;
                s_a *= d_pshape[i];
            }

            memcpy(rchunk + d_coord_f * array->itemsize, src_b + s_coord_f * array->itemsize,
                   seq_copylen);
        }
    }
    return CATERVA_SUCCEED;
}

int caterva_blosc_array_append(caterva_ctx_t *ctx, caterva_array_t *array, void *chunk,
                               int32_t chunksize) {
    CATERVA_UNUSED_PARAM(ctx);

    uint8_t *bchunk = (uint8_t *) chunk;
    int64_t typesize = array->itemsize;
    int32_t size_rep = (int32_t)(array->extchunknitems * typesize);
    int8_t *rchunk = ctx->cfg->alloc((size_t) size_rep);
    int64_t c_pshape[CATERVA_MAX_DIM];
    int8_t c_ndim = array->ndim;

    bool padding = false;
    int32_t size_chunk = array->chunknitems * array->itemsize;
    if (chunksize != size_chunk) {
        padding = true;
    }

    if (padding) {
        uint8_t *paddedchunk = ctx->cfg->alloc(size_chunk);
        memset(paddedchunk, 0, size_chunk);
        int32_t next_pshape[CATERVA_MAX_DIM];
        for (int i = 0; i < CATERVA_MAX_DIM; ++i) {
            next_pshape[(CATERVA_MAX_DIM - c_ndim + i) % CATERVA_MAX_DIM] =
                array->next_chunkshape[i];
            c_pshape[(CATERVA_MAX_DIM - c_ndim + i) % CATERVA_MAX_DIM] = array->chunkshape[i];
        }
        int32_t seq_copylen = next_pshape[7] * array->itemsize;
        bool blank;
        int64_t ii[CATERVA_MAX_DIM];
        int ind_src = 0;
        int ind_dest = 0;
        int64_t ncopies = 1;
        for (int i = 0; i < CATERVA_MAX_DIM - 1; ++i) {
            ncopies *= c_pshape[i];
        }
        for (int ncopy = 0; ncopy < ncopies; ++ncopy) {
            index_unidim_to_multidim(CATERVA_MAX_DIM - 1, c_pshape, ncopy, ii);

            // Calculate if line is full of 0s
            blank = false;
            for (int i = 0; i < CATERVA_MAX_DIM - 1; i++) {
                if (ii[i] >= next_pshape[i]) {
                    blank = true;
                    break;
                }
            }
            if (!blank) {
                memcpy(paddedchunk + ind_dest * array->itemsize, bchunk + ind_src * array->itemsize,
                       seq_copylen);
                ind_src += next_pshape[7];
            }
            ind_dest += c_pshape[7];
        }
        caterva_blosc_array_repart_chunk(rchunk, size_rep, paddedchunk, size_chunk, array);
        ctx->cfg->free(paddedchunk);
    } else {
        caterva_blosc_array_repart_chunk(rchunk, size_rep, bchunk, chunksize, array);
    }
    if (blosc2_schunk_append_buffer(array->sc, rchunk, (size_t) size_rep) < 0) {
        CATERVA_ERROR(CATERVA_ERR_BLOSC_FAILED);
    }
    ctx->cfg->free(rchunk);
    // Calculate chunk position in each dimension
    int64_t c_shape[CATERVA_MAX_DIM];
    int64_t c_eshape[CATERVA_MAX_DIM];
    for (int i = 0; i < CATERVA_MAX_DIM; ++i) {
        c_shape[(CATERVA_MAX_DIM - c_ndim + i) % CATERVA_MAX_DIM] = array->shape[i];
        c_eshape[(CATERVA_MAX_DIM - c_ndim + i) % CATERVA_MAX_DIM] = array->extshape[i];
        c_pshape[(CATERVA_MAX_DIM - c_ndim + i) % CATERVA_MAX_DIM] = array->chunkshape[i];
    }

    int64_t aux[CATERVA_MAX_DIM];
    int64_t poschunk[CATERVA_MAX_DIM];
    aux[7] = c_eshape[7] / c_pshape[7];
    for (int i = CATERVA_MAX_DIM - 2; i >= 0; i--) {
        aux[i] = c_eshape[i] / c_pshape[i] * aux[i + 1];
    }
    poschunk[7] = (array->nchunks + 1) % aux[7];
    for (int i = CATERVA_MAX_DIM - 2; i >= 0; i--) {
        poschunk[i] = ((array->nchunks + 1) % aux[i]) / aux[i + 1];
    }

    // Update next_chunkshape, next_chunknitems
    array->next_chunknitems = 1;
    int64_t n_pshape[CATERVA_MAX_DIM];
    for (int i = 0; i < CATERVA_MAX_DIM; ++i) {
        n_pshape[i] = c_pshape[i];
        if ((poschunk[i] >= (c_eshape[i] / c_pshape[i]) - 1) && (c_eshape[i] > c_shape[i])) {
            n_pshape[i] -= c_eshape[i] - c_shape[i];
        }
        array->next_chunknitems *= n_pshape[i];
    }
    for (int i = 0; i < CATERVA_MAX_DIM; ++i) {
        array->next_chunkshape[i] =
            (int32_t) n_pshape[(CATERVA_MAX_DIM - c_ndim + i) % CATERVA_MAX_DIM];
    }

    return CATERVA_SUCCEED;
}

int caterva_blosc_array_from_buffer(caterva_ctx_t *ctx, caterva_array_t *array, void *buffer,
                                    int64_t buffersize) {
    CATERVA_UNUSED_PARAM(buffersize);

    const int8_t *bbuffer = (int8_t *) buffer;

    int64_t d_shape[CATERVA_MAX_DIM];
    int64_t d_eshape[CATERVA_MAX_DIM];
    int32_t d_pshape[CATERVA_MAX_DIM];
    int8_t d_ndim = array->ndim;

    for (int i = 0; i < CATERVA_MAX_DIM; ++i) {
        d_shape[(CATERVA_MAX_DIM - d_ndim + i) % CATERVA_MAX_DIM] = array->shape[i];
        d_eshape[(CATERVA_MAX_DIM - d_ndim + i) % CATERVA_MAX_DIM] = array->extshape[i];
        d_pshape[(CATERVA_MAX_DIM - d_ndim + i) % CATERVA_MAX_DIM] = array->chunkshape[i];
    }

    int8_t typesize = array->itemsize;
    int8_t *chunk = ctx->cfg->alloc((size_t) array->chunknitems * typesize);
    int8_t *rchunk = ctx->cfg->alloc((size_t) array->extchunknitems * typesize);
    CATERVA_ERROR_NULL(chunk);

    /* Calculate the constants out of the for  */
    int64_t aux[CATERVA_MAX_DIM];
    aux[7] = d_eshape[7] / d_pshape[7];
    for (int i = CATERVA_MAX_DIM - 2; i >= 0; i--) {
        aux[i] = d_eshape[i] / d_pshape[i] * aux[i + 1];
    }

    /* Fill each chunk buffer */
    int64_t desp[CATERVA_MAX_DIM];
    int64_t actual_psize[CATERVA_MAX_DIM];
    for (int64_t ci = 0; ci < array->extnitems / array->chunknitems; ci++) {
        if (!array->filled) {
            memset(chunk, 0, array->chunknitems * typesize);
            memset(rchunk, 0, (size_t) array->extchunknitems * typesize);
            /* Calculate the coord. of the chunk first element */
            desp[7] = ci % (d_eshape[7] / d_pshape[7]) * d_pshape[7];
            for (int i = CATERVA_MAX_DIM - 2; i >= 0; i--) {
                desp[i] = ci % (aux[i]) / (aux[i + 1]) * d_pshape[i];
            }
            /* Calculate if padding with 0s is needed for this chunk */
            for (int i = CATERVA_MAX_DIM - 1; i >= 0; i--) {
                if (desp[i] + d_pshape[i] > d_shape[i]) {
                    actual_psize[i] = (int32_t)(d_shape[i] - desp[i]);
                } else {
                    actual_psize[i] = d_pshape[i];
                }
            }
            int32_t seq_copylen = actual_psize[7] * typesize;
            /* Copy each line of data from chunk to arr */
            int64_t ii[CATERVA_MAX_DIM];
            int64_t ncopies = 1;
            for (int i = 0; i < CATERVA_MAX_DIM - 1; ++i) {
                ncopies *= actual_psize[i];
            }
            for (int ncopy = 0; ncopy < ncopies; ++ncopy) {
                index_unidim_to_multidim(CATERVA_MAX_DIM - 1, actual_psize, ncopy, ii);

                int64_t d_a = d_pshape[7];
                int64_t d_coord_f = 0;
                for (int i = CATERVA_MAX_DIM - 2; i >= 0; i--) {
                    d_coord_f += ii[i] * d_a;
                    d_a *= d_pshape[i];
                }
                int64_t s_coord_f = desp[7];
                int64_t s_a = d_shape[7];
                for (int i = CATERVA_MAX_DIM - 2; i >= 0; i--) {
                    s_coord_f += (desp[i] + ii[i]) * s_a;
                    s_a *= d_shape[i];
                }
                memcpy(chunk + d_coord_f * typesize, bbuffer + s_coord_f * typesize, seq_copylen);
            }
            // Copy each chunk from rchunk to dest
            caterva_blosc_array_repart_chunk(rchunk, (int32_t) array->extchunknitems * typesize,
                                             chunk, array->chunknitems * typesize, array);

            blosc2_schunk_append_buffer(array->sc, rchunk,
                                        (size_t) array->extchunknitems * typesize);
            array->empty = false;
            array->nchunks++;
            if (array->nchunks == array->extnitems / array->chunknitems) {
                array->filled = true;
            }
        }
    }
    ctx->cfg->free(chunk);
    ctx->cfg->free(rchunk);

    return CATERVA_SUCCEED;
}

int caterva_blosc_array_get_slice_buffer(caterva_ctx_t *ctx, caterva_array_t *array,
                                         int64_t *start, int64_t *stop, const int64_t *shape,
                                         void *buffer) {
    uint8_t *bbuffer = buffer;  // for allowing pointer arithmetic
    int64_t start__[CATERVA_MAX_DIM];
    int64_t stop__[CATERVA_MAX_DIM];
    int64_t shape__[CATERVA_MAX_DIM];
    int64_t extshape__[CATERVA_MAX_DIM];
    int64_t chunkshape__[CATERVA_MAX_DIM];
    int64_t extchunkshape__[CATERVA_MAX_DIM];
    int64_t blockshape__[CATERVA_MAX_DIM];

    for (int i = 0; i < CATERVA_MAX_DIM; ++i) {
        start__[i] = (i < array->ndim) ? start[i] : 0;
        stop__[i] = (i < array->ndim) ? stop[i] : 1;
        shape__[i] = (i < array->ndim) ? shape[i] : 1;
        extshape__[i] = (i < array->ndim) ? array->extshape[i] : 1;
        chunkshape__[i] = (i < array->ndim) ? array->chunkshape[i] : 1;
        extchunkshape__[i] = (i < array->ndim) ? array->extchunkshape[i] : 1;
        blockshape__[i] = (i < array->ndim) ? array->blockshape[i] : 1;
    }

    int64_t start_[CATERVA_MAX_DIM];
    int64_t stop_[CATERVA_MAX_DIM];
    int64_t d_pshape_[CATERVA_MAX_DIM];
    int64_t s_pshape[CATERVA_MAX_DIM];
    int64_t s_eshape[CATERVA_MAX_DIM];
    int64_t s_epshape[CATERVA_MAX_DIM];
    int64_t s_spshape[CATERVA_MAX_DIM];
    int8_t s_ndim = array->ndim;

    for (int i = 0; i < CATERVA_MAX_DIM; ++i) {
        start_[(CATERVA_MAX_DIM - s_ndim + i) % CATERVA_MAX_DIM] = start__[i];
        stop_[(CATERVA_MAX_DIM - s_ndim + i) % CATERVA_MAX_DIM] = stop__[i];
        d_pshape_[(CATERVA_MAX_DIM - s_ndim + i) % CATERVA_MAX_DIM] = shape__[i];
        s_eshape[(CATERVA_MAX_DIM - s_ndim + i) % CATERVA_MAX_DIM] = extshape__[i];
        s_pshape[(CATERVA_MAX_DIM - s_ndim + i) % CATERVA_MAX_DIM] = chunkshape__[i];
        s_epshape[(CATERVA_MAX_DIM - s_ndim + i) % CATERVA_MAX_DIM] = extchunkshape__[i];
        s_spshape[(CATERVA_MAX_DIM - s_ndim + i) % CATERVA_MAX_DIM] = blockshape__[i];
    }

    // Acceleration path for the case where we are doing (1-dim) aligned chunk reads
    if ((s_ndim == 1) && (array->chunkshape[0] == shape[0]) &&
        (array->chunkshape[0] == array->blockshape[0]) && (start[0] % array->chunkshape[0] == 0) &&
        (stop[0] % array->chunkshape[0] == 0)) {
        int nchunk = (int) (start[0] / array->chunkshape[0]);
        // In case of an aligned read, decompress directly in destination
        if (blosc2_schunk_decompress_chunk(array->sc, nchunk, bbuffer,
                                           (size_t) array->chunknitems * array->sc->typesize) < 0) {
            CATERVA_ERROR(CATERVA_ERR_BLOSC_FAILED);
        }
        return CATERVA_SUCCEED;
    }

    for (int j = 0; j < CATERVA_MAX_DIM - s_ndim; ++j) {
        start_[j] = 0;
    }
    /* Create chunk buffers */
    int typesize = array->itemsize;
    int nblocks = ((int) array->extchunknitems) / array->blocknitems;
    bool *block_maskout = ctx->cfg->alloc(nblocks);

    uint8_t *chunk;
    bool local_cache;
    if (array->chunk_cache.data == NULL) {
        chunk = (uint8_t *) ctx->cfg->alloc((size_t) array->extchunknitems * typesize);
        CATERVA_ERROR_NULL(chunk);
        local_cache = true;
    } else {
        chunk = array->chunk_cache.data;
        local_cache = false;
    }
    int64_t i_start[CATERVA_MAX_DIM], i_stop[CATERVA_MAX_DIM], i_shape[CATERVA_MAX_DIM];
    for (int i = 0; i < CATERVA_MAX_DIM; ++i) {
        i_start[i] = start_[i] / s_pshape[i];
        i_stop[i] = (stop_[i] - 1) / s_pshape[i];
        i_shape[i] = i_stop[i] - i_start[i] + 1;
    }

    /* Calculate the used chunks */
    int64_t ii[CATERVA_MAX_DIM];
    int64_t j_start[CATERVA_MAX_DIM], j_stop[CATERVA_MAX_DIM], j_shape[CATERVA_MAX_DIM];
    int64_t sp_start[CATERVA_MAX_DIM], sp_stop[CATERVA_MAX_DIM], sp_shape[CATERVA_MAX_DIM];

    int64_t nchunks = 1;
    for (int i = 0; i < CATERVA_MAX_DIM; ++i) {
        nchunks *= i_shape[i];
    }
    for (int chunk_ind = 0; chunk_ind < nchunks; ++chunk_ind) {
        index_unidim_to_multidim(CATERVA_MAX_DIM, i_shape, chunk_ind, ii);
        for (int i = 0; i < CATERVA_MAX_DIM; ++i) {
            ii[i] += i_start[i];
        }

        /* Get the chunk ii */
        memset(block_maskout, true, nblocks);
        int nchunk = 0;
        int inc = 1;
        for (int i = CATERVA_MAX_DIM - 1; i >= 0; --i) {
            nchunk += (int) (ii[i] * inc);
            inc *= (int) (s_eshape[i] / s_pshape[i]);
        }
        if (array->chunk_cache.data != NULL) {
            array->chunk_cache.nchunk = nchunk;
        }
        /* Calculate the used blocks */
        for (int i = 0; i < CATERVA_MAX_DIM; ++i) {
            if (ii[i] == i_start[i]) {
                j_start[i] = (start_[i] % s_pshape[i]) / s_spshape[i];
            } else {
                j_start[i] = 0;
            }
            if (ii[i] == i_stop[i]) {
                j_stop[i] = ((stop_[i] - 1) % s_pshape[i]) / s_spshape[i];
            } else {
                j_stop[i] = (s_epshape[i] / s_spshape[i]) - 1;
            }
            j_shape[i] = j_stop[i] - j_start[i] + 1;
        }

        int64_t jj[CATERVA_MAX_DIM];
        int64_t num_blocks = 1;
        for (int i = 0; i < CATERVA_MAX_DIM; ++i) {
            num_blocks *= j_shape[i];
        }
        for (int block_ind = 0; block_ind < num_blocks; ++block_ind) {
            index_unidim_to_multidim(CATERVA_MAX_DIM, j_shape, block_ind, jj);
            for (int i = 0; i < CATERVA_MAX_DIM; ++i) {
                jj[i] += j_start[i];
            }
            /* Fill chunk mask */
            int sinc = 1;
            int nblock = 0;
            for (int i = CATERVA_MAX_DIM - 1; i >= 0; --i) {
                nblock += (int) (jj[i] * sinc);
                sinc *= (int) (s_epshape[i] / s_spshape[i]);
            }
            block_maskout[nblock] = false;
        }

        blosc2_set_maskout(array->sc->dctx, block_maskout, nblocks);
        blosc2_schunk_decompress_chunk(array->sc, nchunk, chunk,
                                       (size_t) array->extchunknitems * typesize);

        num_blocks = 1;
        for (int i = 0; i < CATERVA_MAX_DIM; ++i) {
            num_blocks *= j_shape[i];
        }
        for (int block_ind = 0; block_ind < num_blocks; ++block_ind) {
            index_unidim_to_multidim(CATERVA_MAX_DIM, j_shape, block_ind, jj);
            for (int i = 0; i < CATERVA_MAX_DIM; ++i) {
                jj[i] += j_start[i];
            }
            /* Decompress block jj */
            int s_start = 0;
            int sinc = 1;
            int nblock = 0;
            for (int i = CATERVA_MAX_DIM - 1; i >= 0; --i) {
                nblock += (int) (jj[i] * sinc);
                sinc *= (int) (s_epshape[i] / s_spshape[i]);
            }

            s_start = nblock * array->blocknitems;
            /* memcpy */
            for (int i = 0; i < CATERVA_MAX_DIM; ++i) {
                if (jj[i] == j_start[i] && ii[i] == i_start[i]) {
                    sp_start[i] = (start_[i] % s_pshape[i]) % s_spshape[i];
                } else {
                    sp_start[i] = 0;
                }
                if (jj[i] == j_stop[i] && ii[i] == i_stop[i]) {
                    sp_stop[i] = (((stop_[i] - 1) % s_pshape[i]) % s_spshape[i]) + 1;
                } else {
                    sp_stop[i] = s_spshape[i];
                }
                if ((jj[i] + 1) * s_spshape[i] > s_pshape[i]) {  // case padding
                    int64_t lastn = s_pshape[i] % s_spshape[i];
                    if (lastn < sp_stop[i]) {
                        sp_stop[i] = lastn;
                    }
                }
                sp_shape[i] = sp_stop[i] - sp_start[i];
            }
            int64_t kk[CATERVA_MAX_DIM];
            kk[CATERVA_MAX_DIM - 1] = sp_start[CATERVA_MAX_DIM - 1];
            int64_t ncopies = 1;
            for (int i = 0; i < CATERVA_MAX_DIM - 1; ++i) {
                ncopies *= sp_shape[i];
            }
            for (int ncopy = 0; ncopy < ncopies; ++ncopy) {
                index_unidim_to_multidim(CATERVA_MAX_DIM - 1, sp_shape, ncopy, kk);
                for (int i = 0; i < CATERVA_MAX_DIM - 1; ++i) {
                    kk[i] += sp_start[i];
                }

                // Copy each line of data from block to bdest
                int64_t sp_pointer = 0;
                int64_t sp_pointer_inc = 1;
                for (int i = CATERVA_MAX_DIM - 1; i >= 0; --i) {
                    sp_pointer += kk[i] * sp_pointer_inc;
                    sp_pointer_inc *= s_spshape[i];
                }
                int64_t buf_pointer = 0;
                int64_t buf_pointer_inc = 1;
                for (int i = CATERVA_MAX_DIM - 1; i >= 0; --i) {
                    buf_pointer +=
                        (kk[i] + s_spshape[i] * jj[i] + s_pshape[i] * ii[i] - start_[i]) *
                        buf_pointer_inc;
                    buf_pointer_inc *= d_pshape_[i];
                }

                memcpy(&bbuffer[buf_pointer * typesize], &chunk[(s_start + sp_pointer) * typesize],
                       (size_t)(sp_stop[7] - sp_start[7]) * typesize);
            }
        }
    }

    ctx->cfg->free(block_maskout);
    if (local_cache) {
        ctx->cfg->free(chunk);
    }
    return CATERVA_SUCCEED;
}

int caterva_blosc_array_to_buffer(caterva_ctx_t *ctx, caterva_array_t *array, void *buffer) {
    int8_t *bbuffer = (int8_t *) buffer;
    int8_t ndim = array->ndim;
    int64_t start[CATERVA_MAX_DIM];
    int64_t stop[CATERVA_MAX_DIM];
    for (int i = 0; i < ndim; i++) {
        start[i] = 0;
        stop[i] = array->shape[i];
    }

    caterva_blosc_array_get_slice_buffer(ctx, array, start, stop, array->shape, bbuffer);
    return CATERVA_SUCCEED;
}

int caterva_blosc_array_get_slice(caterva_ctx_t *ctx, caterva_array_t *src, int64_t *start,
                                  int64_t *stop, caterva_array_t *array) {
    int typesize = src->itemsize;

    uint8_t *chunk = ctx->cfg->alloc((size_t) array->chunknitems * typesize);
    CATERVA_ERROR_NULL(chunk);
    int64_t next_chunkshape__[CATERVA_MAX_DIM];
    int64_t chunkshape__[CATERVA_MAX_DIM];
    int64_t start__[CATERVA_MAX_DIM];
    int64_t stop__[CATERVA_MAX_DIM];

    for (int i = 0; i < CATERVA_MAX_DIM; ++i) {
        start__[i] = (i < src->ndim) ? start[i] : 0;
        stop__[i] = (i < src->ndim) ? stop[i] : 1;
        next_chunkshape__[i] = (i < src->ndim) ? array->next_chunkshape[i] : 1;
        chunkshape__[i] = (i < src->ndim) ? array->chunkshape[i] : 1;
    }

    int64_t d_next_pshape[CATERVA_MAX_DIM];
    int64_t d_pshape[CATERVA_MAX_DIM];
    int64_t d_start[CATERVA_MAX_DIM];
    int64_t d_stop[CATERVA_MAX_DIM];
    int64_t d_shape[CATERVA_MAX_DIM];
    int8_t d_ndim = array->ndim;

    for (int i = 0; i < CATERVA_MAX_DIM; ++i) {
        d_next_pshape[(CATERVA_MAX_DIM - d_ndim + i) % CATERVA_MAX_DIM] = next_chunkshape__[i];
        d_pshape[(CATERVA_MAX_DIM - d_ndim + i) % CATERVA_MAX_DIM] = chunkshape__[i];
        d_start[(CATERVA_MAX_DIM - d_ndim + i) % CATERVA_MAX_DIM] = start__[i];
        d_stop[(CATERVA_MAX_DIM - d_ndim + i) % CATERVA_MAX_DIM] = stop__[i];
    }
    for (int i = 0; i < CATERVA_MAX_DIM; ++i) {
        if (d_next_pshape[i] != 0) {
            d_shape[i] = (d_stop[i] - d_start[i]) / d_next_pshape[i];
            if ((d_stop[i] - d_start[i]) % d_next_pshape[i] != 0) {
                d_shape[i] += 1;
            }
        } else {
            d_shape[i] = 0;
        }
    }

    int64_t nchunks = 1;
    for (int i = 0; i < CATERVA_MAX_DIM; ++i) {
        nchunks *= d_shape[i];
    }
    int64_t ii[CATERVA_MAX_DIM];

    int64_t appended_shape[CATERVA_MAX_DIM] = {0};

    for (int chunk_ind = 0; chunk_ind < nchunks; ++chunk_ind) {
        index_unidim_to_multidim(CATERVA_MAX_DIM, d_shape, chunk_ind, ii);
        for (int j = 0; j < CATERVA_MAX_DIM; ++j) {
            ii[j] *= d_pshape[j];
            ii[j] += d_start[j];
        }

        memset(chunk, 0, array->chunknitems * typesize);
        int64_t jj[CATERVA_MAX_DIM];
        for (int i = 0; i < CATERVA_MAX_DIM; ++i) {
            if (ii[i] + d_next_pshape[i] > d_stop[i]) {
                jj[i] = d_stop[i];
            } else {
                jj[i] = ii[i] + d_next_pshape[i];
            }
        }
        int64_t start_[CATERVA_MAX_DIM];
        int64_t stop_[CATERVA_MAX_DIM];
        int64_t d_pshape_[CATERVA_MAX_DIM];
        for (int i = 0; i < CATERVA_MAX_DIM; ++i) {
            appended_shape[i] = d_next_pshape[i];
            start_[i] = ii[(CATERVA_MAX_DIM - d_ndim + i) % CATERVA_MAX_DIM];
            stop_[i] = jj[(CATERVA_MAX_DIM - d_ndim + i) % CATERVA_MAX_DIM];
            d_pshape_[i] = d_next_pshape[(CATERVA_MAX_DIM - d_ndim + i) % CATERVA_MAX_DIM];
        }

        CATERVA_ERROR(caterva_get_slice_buffer(ctx, src, start_, stop_, d_pshape_, chunk,
                                               array->next_chunknitems * typesize));

        CATERVA_ERROR(caterva_append(ctx, array, chunk, array->next_chunknitems * typesize));
        for (int i = 0; i < src->ndim; ++i) {
            d_next_pshape[(CATERVA_MAX_DIM - d_ndim + i) % CATERVA_MAX_DIM] =
                array->next_chunkshape[i];
        }
    }
    ctx->cfg->free(chunk);

    return CATERVA_SUCCEED;
}

int caterva_blosc_update_shape(caterva_array_t *array, int8_t ndim, int64_t *shape,
                               int32_t *chunkshape, int32_t *blockshape) {
    array->ndim = ndim;
    array->nitems = 1;
    array->extnitems = 1;
    array->extchunknitems = 1;
    array->chunknitems = 1;
    array->blocknitems = 1;
    for (int i = 0; i < CATERVA_MAX_DIM; ++i) {
        if (i < ndim) {
            array->shape[i] = shape[i];
            array->chunkshape[i] = chunkshape[i];
            array->blockshape[i] = blockshape[i];
            if (shape[i] != 0) {
                if (shape[i] % array->chunkshape[i] == 0) {
                    array->extshape[i] = shape[i];
                } else {
                    array->extshape[i] = shape[i] + chunkshape[i] - shape[i] % chunkshape[i];
                }
                if (chunkshape[i] % blockshape[i] == 0) {
                    array->extchunkshape[i] = chunkshape[i];
                } else {
                    array->extchunkshape[i] =
                            chunkshape[i] + blockshape[i] - chunkshape[i] % blockshape[i];
                }
            } else {
                array->extchunkshape[i] = 0;
                array->extshape[i] = 0;
            }
        } else {
            array->blockshape[i] = 1;
            array->chunkshape[i] = 1;
            array->extshape[i] = 1;
            array->extchunkshape[i] = 1;
            array->shape[i] = 1;
        }
        array->nitems *= array->shape[i];
        array->extnitems *= array->extshape[i];
        array->extchunknitems *= array->extchunkshape[i];
        array->chunknitems *= array->chunkshape[i];
        array->blocknitems *= array->blockshape[i];
    }

    uint8_t *smeta = NULL;
    // Serialize the dimension info ...
    int32_t smeta_len =
        serialize_meta(array->ndim, array->shape, array->chunkshape, array->blockshape, &smeta);
    if (smeta_len < 0) {
        fprintf(stderr, "error during serializing dims info for Caterva");
        return -1;
    }
    // ... and update it in its metalayer
    if (blosc2_meta_exists(array->sc, "caterva") < 0) {
        if (blosc2_meta_add(array->sc, "caterva", smeta, (uint32_t) smeta_len) < 0) {
            CATERVA_ERROR(CATERVA_ERR_BLOSC_FAILED);
        }
    } else {
        if (blosc2_meta_update(array->sc, "caterva", smeta, (uint32_t) smeta_len) < 0) {
            CATERVA_ERROR(CATERVA_ERR_BLOSC_FAILED);
        }
    }
    free(smeta);

    return CATERVA_SUCCEED;
}


int caterva_blosc_array_squeeze_index(caterva_ctx_t *ctx, caterva_array_t *array, bool *index) {
    CATERVA_UNUSED_PARAM(ctx);
    uint8_t nones = 0;
    int64_t newshape[CATERVA_MAX_DIM];
    int32_t newchunkshape[CATERVA_MAX_DIM];
    int32_t newblockshape[CATERVA_MAX_DIM];

    for (int i = 0; i < array->ndim; ++i) {
        if (index[i] == true) {
            if (array->shape[i] != 1) {
                CATERVA_ERROR(CATERVA_ERR_INVALID_INDEX);
            }
        } else {
            newshape[nones] = array->shape[i];
            newchunkshape[nones] = array->chunkshape[i];
            newblockshape[nones] = array->blockshape[i];
            nones += 1;
        }
    }

    for (int i = 0; i < CATERVA_MAX_DIM; ++i) {
        if (i < nones) {
            array->chunkshape[i] = newchunkshape[i];
            array->blockshape[i] = newblockshape[i];
        } else {
            array->chunkshape[i] = 1;
            array->blockshape[i] = 1;
        }
    }

    CATERVA_ERROR(caterva_blosc_update_shape(array, nones, newshape, newchunkshape, newblockshape));

    return CATERVA_SUCCEED;
}

int caterva_blosc_array_squeeze(caterva_ctx_t *ctx, caterva_array_t *array) {
    CATERVA_UNUSED_PARAM(ctx);
    int32_t newblockshape[CATERVA_MAX_DIM];
    bool index[CATERVA_MAX_DIM];

    for (int i = 0; i < array->ndim; ++i) {
        if (array->shape[i] != 1) {
           index[i] = false;
        } else {
            index[i] = true;
        }
    }
    CATERVA_ERROR(caterva_blosc_array_squeeze_index(ctx, array, index));

    return CATERVA_SUCCEED;
}

int caterva_blosc_array_copy(caterva_ctx_t *ctx, caterva_params_t *params,
                             caterva_storage_t *storage, caterva_array_t *src,
                             caterva_array_t **dest) {
    CATERVA_UNUSED_PARAM(params);

    bool equals = true;
    if (src->storage == CATERVA_STORAGE_PLAINBUFFER) {
        equals = false;
    }
    for (int i = 0; i < src->ndim; ++i) {
        if (src->chunkshape[i] != storage->properties.blosc.chunkshape[i]) {
            equals = false;
            break;
        }
        if (src->blockshape[i] != storage->properties.blosc.blockshape[i]) {
            equals = false;
            break;
        }
    }

    if (equals) {
        CATERVA_ERROR(caterva_empty(ctx, params, storage, dest));
        blosc2_schunk *new_sc = blosc2_schunk_copy(src->sc, (*dest)->sc->storage);
        blosc2_schunk_free((*dest)->sc);
        (*dest)->sc = new_sc;
        src->empty = false;
        src->filled = true;
    } else {
        int64_t start[CATERVA_MAX_DIM] = {0, 0, 0, 0, 0, 0, 0, 0};

        int64_t stop[CATERVA_MAX_DIM];
        for (int i = 0; i < src->ndim; ++i) {
            stop[i] = src->shape[i];
        }
        CATERVA_ERROR(caterva_get_slice(ctx, src, start, stop, storage, dest));
    }

    return CATERVA_SUCCEED;
}

int caterva_blosc_array_empty(caterva_ctx_t *ctx, caterva_params_t *params,
                              caterva_storage_t *storage, caterva_array_t **array) {
    /* Create a caterva_array_t buffer */
    (*array) = (caterva_array_t *) ctx->cfg->alloc(sizeof(caterva_array_t));
    if ((*array) == NULL) {
        DEBUG_PRINT("Pointer is null");
        return CATERVA_ERR_NULL_POINTER;
    }

    (*array)->storage = storage->backend;
    (*array)->ndim = params->ndim;
    (*array)->itemsize = params->itemsize;

    int64_t *shape = params->shape;
    int32_t *chunkshape = storage->properties.blosc.chunkshape;
    int32_t *blockshape = storage->properties.blosc.blockshape;
    (*array)->nitems = 1;
    (*array)->chunknitems = 1;
    (*array)->extnitems = 1;
    (*array)->blocknitems = 1;
    (*array)->extchunknitems = 1;

    for (int i = 0; i < params->ndim; ++i) {
        (*array)->shape[i] = shape[i];
        (*array)->chunkshape[i] = chunkshape[i];
        (*array)->next_chunkshape[i] = chunkshape[i];
        (*array)->blockshape[i] = blockshape[i];
        if (shape[i] != 0) {
            if (shape[i] % chunkshape[i] == 0) {
                (*array)->extshape[i] = shape[i];
            } else {
                (*array)->extshape[i] = shape[i] + chunkshape[i] - shape[i] % chunkshape[i];
            }
            if (chunkshape[i] % blockshape[i] == 0) {
                (*array)->extchunkshape[i] = chunkshape[i];
            } else {
                (*array)->extchunkshape[i] =
                        chunkshape[i] + blockshape[i] - chunkshape[i] % blockshape[i];
            }
        } else {
            (*array)->extshape[i] = 0;
            (*array)->extchunkshape[i] = 0;
        }
        (*array)->nitems *= shape[i];
        (*array)->chunknitems *= chunkshape[i];
        (*array)->blocknitems *= blockshape[i];
        (*array)->extnitems *= (*array)->extshape[i];
        (*array)->extchunknitems *= (*array)->extchunkshape[i];
    }
    (*array)->next_chunknitems = (*array)->chunknitems;


    for (int i = params->ndim; i < CATERVA_MAX_DIM; ++i) {
        (*array)->shape[i] = 1;
        (*array)->chunkshape[i] = 1;
        (*array)->extshape[i] = 1;
        (*array)->blockshape[i] = 1;
        (*array)->extchunkshape[i] = 1;
        (*array)->next_chunkshape[i] = 1;
    }

    // The partition cache (empty initially)
    (*array)->chunk_cache.data = NULL;
    (*array)->chunk_cache.nchunk = -1;  // means no valid cache yet

    (*array)->buf = NULL;

    blosc2_cparams cparams = BLOSC2_CPARAMS_DEFAULTS;
    cparams.blocksize = (*array)->blocknitems * params->itemsize;
    cparams.schunk = NULL;
    cparams.typesize = params->itemsize;
    cparams.prefilter = ctx->cfg->prefilter;
    cparams.pparams = ctx->cfg->pparams;
    cparams.use_dict = ctx->cfg->usedict;
    cparams.nthreads = (int16_t) ctx->cfg->nthreads;
    cparams.clevel = (uint8_t) ctx->cfg->complevel;
    cparams.compcode = (uint8_t) ctx->cfg->compcodec;
    for (int i = 0; i < BLOSC2_MAX_FILTERS; ++i) {
        cparams.filters[i] = ctx->cfg->filters[i];
        cparams.filters_meta[i] = ctx->cfg->filtersmeta[i];
    }

    blosc2_dparams dparams = BLOSC2_DPARAMS_DEFAULTS;
    dparams.schunk = NULL;
    dparams.nthreads = ctx->cfg->nthreads;

    blosc2_storage b_storage = BLOSC2_STORAGE_DEFAULTS;
    b_storage.cparams = &cparams;
    b_storage.dparams = &dparams;

    if (storage->properties.blosc.sequencial) {
        b_storage.contiguous = true;
    }
    if (storage->properties.blosc.urlpath != NULL) {
        b_storage.urlpath = storage->properties.blosc.urlpath;
    }

    blosc2_schunk *sc = blosc2_schunk_new(&b_storage);

    if (sc == NULL) {
        DEBUG_PRINT("Pointer is NULL");
        return CATERVA_ERR_BLOSC_FAILED;
    }

    // Serialize the dimension info
    if (sc->nmetalayers >= BLOSC2_MAX_METALAYERS) {
        DEBUG_PRINT("the number of metalayers for this schunk has been exceeded");
        return CATERVA_ERR_BLOSC_FAILED;
    }
    uint8_t *smeta = NULL;
    int32_t smeta_len = serialize_meta(params->ndim, shape, chunkshape, blockshape, &smeta);
    if (smeta_len < 0) {
        DEBUG_PRINT("error during serializing dims info for Caterva");
        return CATERVA_ERR_BLOSC_FAILED;
    }

    // And store it in caterva metalayer
    if (blosc2_meta_add(sc, "caterva", smeta, (uint32_t) smeta_len) < 0) {
        return CATERVA_ERR_BLOSC_FAILED;
    }

    free(smeta);

    for (int i = 0; i < storage->properties.blosc.nmetalayers; ++i) {
        char *name = storage->properties.blosc.metalayers[i].name;
        uint8_t *data = storage->properties.blosc.metalayers[i].sdata;
        int32_t size = storage->properties.blosc.metalayers[i].size;
        blosc2_meta_add(sc, name, data, size);
    }
    (*array)->sc = sc;

    return CATERVA_SUCCEED;
}
