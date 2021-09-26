/*!
 * bip152.c - compact blocks for libsatoshi
 * Copyright (c) 2021, Christopher Jeffrey (MIT License).
 * https://github.com/chjj/libsatoshi
 */

#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <satoshi/bip152.h>
#include <satoshi/block.h>
#include <satoshi/consensus.h>
#include <satoshi/crypto/hash.h>
#include <satoshi/crypto/rand.h>
#include <satoshi/crypto/siphash.h>
#include <satoshi/header.h>
#include <satoshi/map.h>
#include <satoshi/tx.h>
#include <satoshi/util.h>
#include <satoshi/vector.h>

#include "impl.h"
#include "internal.h"

/*
 * ID Vector
 */

static void
idvec_init(btc_idvec_t *z) {
  z->items = NULL;
  z->alloc = 0;
  z->length = 0;
}

static void
idvec_clear(btc_idvec_t *z) {
  if (z->alloc > 0)
    free(z->items);

  z->items = NULL;
  z->alloc = 0;
  z->length = 0;
}

static void
idvec_grow(btc_idvec_t *z, size_t zn) {
  if (zn > z->alloc) {
    z->items = (uint64_t *)btc_realloc(z->items, zn * sizeof(uint64_t));
    z->alloc = zn;
  }
}

static void
idvec_push(btc_idvec_t *z, uint64_t x) {
  if (z->length == z->alloc)
    idvec_grow(z, (z->alloc * 3) / 2 + (z->alloc <= 1));

  z->items[z->length++] = x;
}

/*
 * Compact Block
 */

DEFINE_SERIALIZABLE_OBJECT(btc_cmpct, SCOPE_EXTERN)

void
btc_cmpct_init(btc_cmpct_t *z) {
  btc_hash_init(z->hash);
  btc_header_init(&z->header);
  z->key_nonce = 0;
  idvec_init(&z->ids);
  btc_txvec_init(&z->ptx);
  btc_vector_init(&z->avail);
  z->id_map = btc_longtab_create();
  z->count = 0;
  btc_hash_init(z->sipkey);
  z->now = 0;
}

void
btc_cmpct_clear(btc_cmpct_t *z) {
  size_t i;

  btc_header_clear(&z->header);

  idvec_clear(&z->ids);

  btc_txvec_clear(&z->ptx);

  for (i = 0; i < z->avail.length; i++) {
    if (z->avail.items[i] != NULL)
      btc_tx_destroy((btc_tx_t *)z->avail.items[i]);
  }

  btc_vector_clear(&z->avail);

  btc_longtab_destroy(z->id_map);
}

void
btc_cmpct_copy(btc_cmpct_t *z, const btc_cmpct_t *x) {
  (void)z;
  (void)x;
  btc_abort(); /* LCOV_EXCL_LINE */
}

uint64_t
btc_cmpct_sid(const btc_cmpct_t *blk, const uint8_t *hash) {
  return btc_siphash_sum(hash, 32, blk->sipkey) & UINT64_C(0xffffffffffff);
}

static void
btc_cmpct_key(uint8_t *key, const btc_cmpct_t *blk) {
  uint8_t data[88];

  btc_header_write(data, &blk->header);
  btc_uint64_write(data + 80, blk->key_nonce);

  btc_sha256(key, data, 88);
}

void
btc_cmpct_set_block(btc_cmpct_t *z, const btc_block_t *x, int witness) {
  uint8_t hash[32];
  btc_tx_t *cb;
  size_t i;

  CHECK(x->txs.length > 0);

  btc_header_hash(z->hash, &x->header);
  btc_header_copy(&z->header, &x->header);

  z->key_nonce = ((uint64_t)btc_random() << 32) | btc_random();

  btc_cmpct_key(z->sipkey, z);

  CHECK(z->ids.length == 0);

  for (i = 1; i < x->txs.length; i++) {
    const btc_tx_t *tx = x->txs.items[i];

    if (witness)
      btc_tx_wtxid(hash, tx);
    else
      btc_tx_txid(hash, tx);

    idvec_push(&z->ids, btc_cmpct_sid(z, hash));
  }

  CHECK(z->ptx.length == 0);

  cb = btc_tx_clone(x->txs.items[0]);
  cb->_index = 0;

  btc_txvec_clear(&z->ptx);
  btc_txvec_push(&z->ptx, cb);
}

int
btc_cmpct_setup(btc_cmpct_t *blk) {
  size_t total = blk->ptx.length + blk->ids.length;
  int offset = 0;
  int last = -1;
  size_t i;

  if (total == 0)
    return -1;

  if (total > BTC_MAX_BLOCK_SIZE / 10)
    return -1;

  /* Custom limit to avoid a hashdos. */
  if (total > (BTC_MAX_BLOCK_SIZE - 81) / 60)
    return -1;

  CHECK(blk->avail.length == 0);
  CHECK(blk->count == 0);

  btc_vector_resize(&blk->avail, total);

  for (i = 0; i < blk->avail.length; i++)
    blk->avail.items[i] = NULL;

  for (i = 0; i < blk->ptx.length; i++) {
    const btc_tx_t *tx = blk->ptx.items[i];

    last += tx->_index + 1;

    if (last < 0 || last > 0xffff)
      return -1;

    if ((size_t)last > blk->ids.length + i)
      return -1;

    blk->avail.items[last] = btc_tx_clone(tx);
    blk->count += 1;
  }

  CHECK(btc_longtab_size(blk->id_map) == 0);

  for (i = 0; i < blk->ids.length; i++) {
    uint64_t id = blk->ids.items[i];

    while (blk->avail.items[i + offset] != NULL)
      offset += 1;

    if (!btc_longtab_put(blk->id_map, id, i + offset)) {
      /* Fails on siphash collision. */
      return 0;
    }
  }

  return 1;
}

int
btc_cmpct_fill_missing(btc_cmpct_t *blk, const btc_blocktxn_t *msg) {
  size_t total = blk->ptx.length + blk->ids.length;
  size_t offset = 0;
  size_t i;

  CHECK(blk->avail.length == total);

  for (i = 0; i < blk->avail.length; i++) {
    if (blk->avail.items[i] != NULL)
      continue;

    if (offset >= msg->txs.length)
      return 0;

    blk->avail.items[i] = btc_tx_clone(msg->txs.items[offset++]);
    blk->count += 1;
  }

  return offset == msg->txs.length;
}

void
btc_cmpct_finalize(btc_block_t *z, btc_cmpct_t *x) {
  size_t total = x->ptx.length + x->ids.length;
  btc_tx_t *tx;
  size_t i;

  CHECK(z->txs.length == 0);
  CHECK(x->avail.length == total);
  CHECK(x->count == total);

  btc_header_copy(&z->header, &x->header);
  btc_txvec_resize(&z->txs, x->avail.length);

  for (i = 0; i < x->avail.length; i++) {
    tx = (btc_tx_t *)x->avail.items[i];

    CHECK(tx != NULL);

    x->avail.items[i] = NULL;
    z->txs.items[i] = tx;
  }
}

static size_t
btc_cmpct__size(const btc_cmpct_t *blk, int witness) {
  size_t size = 0;
  size_t i;

  size += 80;
  size += 8;
  size += btc_size_size(blk->ids.length);
  size += blk->ids.length * 6;
  size += btc_size_size(blk->ptx.length);

  for (i = 0; i < blk->ptx.length; i++) {
    const btc_tx_t *tx = blk->ptx.items[i];

    size += btc_size_size(tx->_index);

    if (witness)
      size += btc_tx_size(tx);
    else
      size += btc_tx_base_size(tx);
  }

  return size;
}

static uint8_t *
btc_cmpct__write(uint8_t *zp, const btc_cmpct_t *x, int witness) {
  size_t i;

  zp = btc_header_write(zp, &x->header);
  zp = btc_uint64_write(zp, x->key_nonce);
  zp = btc_size_write(zp, x->ids.length);

  for (i = 0; i < x->ids.length; i++) {
    uint64_t id = x->ids.items[i];
    uint32_t lo = id & 0xffffffff;
    uint16_t hi = id >> 32;

    zp = btc_uint32_write(zp, lo);
    zp = btc_uint16_write(zp, hi);
  }

  zp = btc_size_write(zp, x->ptx.length);

  for (i = 0; i < x->ptx.length; i++) {
    const btc_tx_t *tx = x->ptx.items[i];

    zp = btc_size_write(zp, tx->_index);

    if (witness)
      zp = btc_tx_write(zp, tx);
    else
      zp = btc_tx_base_write(zp, tx);
  }

  return zp;
}

size_t
btc_cmpct_base_size(const btc_cmpct_t *blk) {
  return btc_cmpct__size(blk, 0);
}

uint8_t *
btc_cmpct_base_write(uint8_t *zp, const btc_cmpct_t *x) {
  return btc_cmpct__write(zp, x, 0);
}

size_t
btc_cmpct_size(const btc_cmpct_t *blk) {
  return btc_cmpct__size(blk, 1);
}

uint8_t *
btc_cmpct_write(uint8_t *zp, const btc_cmpct_t *x) {
  return btc_cmpct__write(zp, x, 1);
}

int
btc_cmpct_read(btc_cmpct_t *z, const uint8_t **xp, size_t *xn) {
  size_t i, idlen, txlen, index;
  btc_tx_t *tx;
  uint32_t lo;
  uint16_t hi;

  if (!btc_header_read(&z->header, xp, xn))
    return 0;

  btc_header_hash(z->hash, &z->header);

  if (!btc_uint64_read(&z->key_nonce, xp, xn))
    return 0;

  btc_cmpct_key(z->sipkey, z);

  if (!btc_size_read(&idlen, xp, xn))
    return 0;

  CHECK(z->ids.length == 0);

  for (i = 0; i < idlen; i++) {
    if (!btc_uint32_read(&lo, xp, xn))
      return 0;

    if (!btc_uint16_read(&hi, xp, xn))
      return 0;

    idvec_push(&z->ids, ((uint64_t)hi << 32) | lo);
  }

  if (!btc_size_read(&txlen, xp, xn))
    return 0;

  CHECK(z->ptx.length == 0);

  for (i = 0; i < txlen; i++) {
    if (!btc_size_read(&index, xp, xn))
      return 0;

    if (index > 0xffff)
      return 0;

    if (index >= txlen + idlen)
      return 0;

    tx = btc_tx_create();

    if (!btc_tx_read(tx, xp, xn)) {
      btc_tx_destroy(tx);
      return 0;
    }

    tx->_index = index;

    btc_txvec_push(&z->ptx, tx);
  }

  return 1;
}

/*
 * TX Request
 */

DEFINE_SERIALIZABLE_OBJECT(btc_getblocktxn, SCOPE_EXTERN)

void
btc_getblocktxn_init(btc_getblocktxn_t *z) {
  btc_hash_init(z->hash);
  idvec_init(&z->indexes);
}

void
btc_getblocktxn_clear(btc_getblocktxn_t *z) {
  idvec_clear(&z->indexes);
}

void
btc_getblocktxn_copy(btc_getblocktxn_t *z, const btc_getblocktxn_t *x) {
  (void)z;
  (void)x;
  btc_abort(); /* LCOV_EXCL_LINE */
}

void
btc_getblocktxn_set_cmpct(btc_getblocktxn_t *z, const btc_cmpct_t *x) {
  size_t i;

  CHECK(z->indexes.length == 0);

  btc_header_hash(z->hash, &x->header);

  for (i = 0; i < x->avail.length; i++) {
    if (x->avail.items[i] == NULL)
      idvec_push(&z->indexes, i);
  }
}

size_t
btc_getblocktxn_size(const btc_getblocktxn_t *x) {
  size_t size = 0;
  size_t i, index;

  size += 32;
  size += btc_size_size(x->indexes.length);

  for (i = 0; i < x->indexes.length; i++) {
    index = x->indexes.items[i];

    if (i > 0)
      index -= x->indexes.items[i - 1] + 1;

    size += btc_size_size(index);
  }

  return size;
}

uint8_t *
btc_getblocktxn_write(uint8_t *zp, const btc_getblocktxn_t *x) {
  size_t i, index;

  zp = btc_raw_write(zp, x->hash, 32);
  zp = btc_size_write(zp, x->indexes.length);

  for (i = 0; i < x->indexes.length; i++) {
    index = x->indexes.items[i];

    if (i > 0)
      index -= x->indexes.items[i - 1] + 1;

    zp = btc_size_write(zp, index);
  }

  return zp;
}

int
btc_getblocktxn_read(btc_getblocktxn_t *z, const uint8_t **xp, size_t *xn) {
  size_t i, count, index;
  size_t offset = 0;

  CHECK(z->indexes.length == 0);

  if (!btc_raw_read(z->hash, 32, xp, xn))
    return 0;

  if (!btc_size_read(&count, xp, xn))
    return 0;

  for (i = 0; i < count; i++) {
    if (!btc_size_read(&index, xp, xn))
      return 0;

    if (index > 0xffff)
      return 0;

    idvec_push(&z->indexes, index);
  }

  for (i = 0; i < count; i++) {
    index = z->indexes.items[i];
    index += offset;

    if (index > 0xffff)
      return 0;

    z->indexes.items[i] = index;

    offset = index + 1;
  }

  return 1;
}

/*
 * TX Response
 */

DEFINE_SERIALIZABLE_OBJECT(btc_blocktxn, SCOPE_EXTERN)

void
btc_blocktxn_init(btc_blocktxn_t *z) {
  btc_hash_init(z->hash);
  btc_txvec_init(&z->txs);
}

void
btc_blocktxn_clear(btc_blocktxn_t *z) {
  btc_txvec_clear(&z->txs);
}

void
btc_blocktxn_copy(btc_blocktxn_t *z, const btc_blocktxn_t *x) {
  btc_hash_copy(z->hash, x->hash);
  btc_txvec_copy(&z->txs, &x->txs);
}

void
btc_blocktxn_set_block(btc_blocktxn_t *res,
                       const btc_block_t *blk,
                       const btc_getblocktxn_t *req) {
  const btc_tx_t *tx;
  size_t i, index;

  CHECK(res->txs.length == 0);

  btc_header_hash(res->hash, &blk->header);

  for (i = 0; i < req->indexes.length; i++) {
    index = req->indexes.items[i];

    if (index >= blk->txs.length)
      break;

    tx = blk->txs.items[index];

    btc_txvec_push(&res->txs, btc_tx_clone(tx));
  }
}

size_t
btc_blocktxn_base_size(const btc_blocktxn_t *x) {
  return 32 + btc_txvec_base_size(&x->txs);
}

uint8_t *
btc_blocktxn_base_write(uint8_t *zp, const btc_blocktxn_t *x) {
  zp = btc_raw_write(zp, x->hash, 32);
  zp = btc_txvec_base_write(zp, &x->txs);
  return zp;
}

size_t
btc_blocktxn_size(const btc_blocktxn_t *x) {
  return 32 + btc_txvec_size(&x->txs);
}

uint8_t *
btc_blocktxn_write(uint8_t *zp, const btc_blocktxn_t *x) {
  zp = btc_raw_write(zp, x->hash, 32);
  zp = btc_txvec_write(zp, &x->txs);
  return zp;
}

int
btc_blocktxn_read(btc_blocktxn_t *z, const uint8_t **xp, size_t *xn) {
  CHECK(z->txs.length == 0);

  if (!btc_raw_read(z->hash, 32, xp, xn))
    return 0;

  if (!btc_txvec_read(&z->txs, xp, xn))
    return 0;

  return 1;
}
