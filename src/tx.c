/*!
 * tx.c - tx for libsatoshi
 * Copyright (c) 2021, Christopher Jeffrey (MIT License).
 * https://github.com/chjj/libsatoshi
 */

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <satoshi/coins.h>
#include <satoshi/consensus.h>
#include <satoshi/crypto/ecc.h>
#include <satoshi/crypto/hash.h>
#include <satoshi/policy.h>
#include <satoshi/script.h>
#include <satoshi/tx.h>
#include "impl.h"
#include "internal.h"
#include "map.h"

/*
 * Transaction
 */

DEFINE_SERIALIZABLE_OBJECT(btc_tx, SCOPE_EXTERN)

void
btc_tx_init(btc_tx_t *tx) {
  tx->version = 1;
  btc_inpvec_init(&tx->inputs);
  btc_outvec_init(&tx->outputs);
  tx->locktime = 0;
}

void
btc_tx_clear(btc_tx_t *tx) {
  btc_inpvec_clear(&tx->inputs);
  btc_outvec_clear(&tx->outputs);
}

void
btc_tx_copy(btc_tx_t *z, const btc_tx_t *x) {
  z->version = x->version;
  btc_inpvec_copy(&z->inputs, &x->inputs);
  btc_outvec_copy(&z->outputs, &x->outputs);
  z->locktime = x->locktime;
}

int
btc_tx_is_coinbase(const btc_tx_t *tx) {
  if (tx->inputs.length != 1)
    return 0;

  return btc_outpoint_is_null(&tx->inputs.items[0]->prevout);
}

int
btc_tx_has_witness(const btc_tx_t *tx) {
  size_t i;

  for (i = 0; i < tx->inputs.length; i++) {
    if (tx->inputs.items[i]->witness.length > 0)
      return 1;
  }

  return 0;
}

static void
btc_tx_digest(uint8_t *hash, const btc_tx_t *tx, int witness) {
  btc_hash256_t ctx;
  size_t i;

  if (witness)
    witness = btc_tx_has_witness(tx);

  btc_hash256_init(&ctx);

  btc_uint32_update(&ctx, tx->version);

  if (witness) {
    btc_uint8_update(&ctx, 0);
    btc_uint8_update(&ctx, 1);
  }

  btc_inpvec_update(&ctx, &tx->inputs);
  btc_outvec_update(&ctx, &tx->outputs);

  if (witness) {
    for (i = 0; i < tx->inputs.length; i++)
      btc_stack_update(&ctx, &tx->inputs.items[i]->witness);
  }

  btc_uint32_update(&ctx, tx->locktime);

  btc_hash256_final(&ctx, hash);
}

void
btc_tx_txid(uint8_t *hash, const btc_tx_t *tx) {
  btc_tx_digest(hash, tx, 0);
}

void
btc_tx_wtxid(uint8_t *hash, const btc_tx_t *tx) {
  btc_tx_digest(hash, tx, 1);
}

static void
btc_tx_sighash_v0(uint8_t *hash,
                  const btc_tx_t *tx,
                  size_t index,
                  const btc_script_t *prev_,
                  unsigned int type) {
  const btc_input_t *input;
  const btc_output_t *output;
  btc_script_t prev;
  btc_hash256_t ctx;
  size_t i;

  if ((type & 0x1f) == BTC_SIGHASH_SINGLE) {
    /* Bitcoind used to return 1 as an error code:
       it ended up being treated like a hash. */
    if (index >= tx->outputs.length) {
      memset(hash, 0, 32);
      hash[0] = 0x01;
      return;
    }
  }

  /* Remove all code separators. */
  btc_script_init(&prev);
  btc_script_remove_separators(&prev, prev_);

  /* Start hashing. */
  btc_hash256_init(&ctx);

  btc_uint32_update(&ctx, tx->version);

  /* Serialize inputs. */
  if (type & BTC_SIGHASH_ANYONECANPAY) {
    /* Serialize only the current
       input if ANYONECANPAY. */
    input = tx->inputs.items[index];

    /* Count. */
    btc_size_update(&ctx, 1);

    /* Outpoint. */
    btc_outpoint_update(&ctx, &input->prevout);

    /* Replace script with previous
       output script if current index. */
    btc_script_update(&ctx, &prev);
    btc_uint32_update(&ctx, input->sequence);
  } else {
    btc_size_update(&ctx, tx->inputs.length);

    for (i = 0; i < tx->inputs.length; i++) {
      input = tx->inputs.items[i];

      /* Outpoint. */
      btc_outpoint_update(&ctx, &input->prevout);

      /* Replace script with previous
         output script if current index. */
      if (i == index) {
        btc_script_update(&ctx, &prev);
        btc_uint32_update(&ctx, input->sequence);
        continue;
      }

      /* Script is null. */
      btc_size_update(&ctx, 0);

      /* Sequences are 0 if NONE or SINGLE. */
      switch (type & 0x1f) {
        case BTC_SIGHASH_NONE:
        case BTC_SIGHASH_SINGLE:
          btc_uint32_update(&ctx, 0);
          break;
        default:
          btc_uint32_update(&ctx, input->sequence);
          break;
      }
    }
  }

  btc_script_clear(&prev);

  /* Serialize outputs. */
  switch (type & 0x1f) {
    case BTC_SIGHASH_NONE: {
      /* No outputs if NONE. */
      btc_size_update(&ctx, 0);
      break;
    }
    case BTC_SIGHASH_SINGLE: {
      output = tx->outputs.items[index];

      /* Drop all outputs after the
         current input index if SINGLE. */
      btc_size_update(&ctx, index + 1);

      for (i = 0; i < index; i++) {
        /* Null all outputs not at
           current input index. */
        btc_int64_update(&ctx, -1);
        btc_size_update(&ctx, 0);
      }

      /* Regular serialization
         at current input index. */
      btc_output_update(&ctx, output);

      break;
    }
    default: {
      /* Regular output serialization if ALL. */
      btc_size_update(&ctx, tx->outputs.length);

      for (i = 0; i < tx->outputs.length; i++) {
        output = tx->outputs.items[i];
        btc_output_update(&ctx, output);
      }

      break;
    }
  }

  btc_uint32_update(&ctx, tx->locktime);

  /* Append the hash type. */
  btc_uint32_update(&ctx, type);

  btc_hash256_final(&ctx, hash);
}

static void
btc_tx_sighash_v1(uint8_t *hash,
                  const btc_tx_t *tx,
                  size_t index,
                  const btc_script_t *prev,
                  int64_t value,
                  unsigned int type,
                  btc_tx_cache_t *cache) {
  const btc_input_t *input = tx->inputs.items[index];
  uint8_t prevouts[32];
  uint8_t sequences[32];
  uint8_t outputs[32];
  btc_hash256_t ctx;
  size_t i;

  memset(prevouts, 0, 32);
  memset(sequences, 0, 32);
  memset(outputs, 0, 32);

  if (!(type & BTC_SIGHASH_ANYONECANPAY)) {
    if (cache != NULL && cache->has_prevouts) {
      memcpy(prevouts, cache->prevouts, 32);
    } else {
      btc_hash256_init(&ctx);

      for (i = 0; i < tx->inputs.length; i++)
        btc_outpoint_update(&ctx, &tx->inputs.items[i]->prevout);

      btc_hash256_final(&ctx, prevouts);

      if (cache != NULL) {
        memcpy(cache->prevouts, prevouts, 32);
        cache->has_prevouts = 1;
      }
    }
  }

  if (!(type & BTC_SIGHASH_ANYONECANPAY)
      && (type & 0x1f) != BTC_SIGHASH_SINGLE
      && (type & 0x1f) != BTC_SIGHASH_NONE) {
    if (cache != NULL && cache->has_sequences) {
      memcpy(sequences, cache->sequences, 32);
    } else {
      btc_hash256_init(&ctx);

      for (i = 0; i < tx->inputs.length; i++)
        btc_uint32_update(&ctx, tx->inputs.items[i]->sequence);

      btc_hash256_final(&ctx, sequences);

      if (cache != NULL) {
        memcpy(cache->sequences, sequences, 32);
        cache->has_sequences = 1;
      }
    }
  }

  if ((type & 0x1f) != BTC_SIGHASH_SINGLE
      && (type & 0x1f) != BTC_SIGHASH_NONE) {
    if (cache != NULL && cache->has_outputs) {
      memcpy(outputs, cache->outputs, 32);
    } else {
      btc_hash256_init(&ctx);

      for (i = 0; i < tx->outputs.length; i++)
        btc_output_update(&ctx, tx->outputs.items[i]);

      btc_hash256_final(&ctx, outputs);

      if (cache != NULL) {
        memcpy(cache->outputs, outputs, 32);
        cache->has_outputs = 1;
      }
    }
  } else if ((type & 0x1f) == BTC_SIGHASH_SINGLE) {
    if (index < tx->outputs.length) {
      btc_hash256_init(&ctx);
      btc_output_update(&ctx, tx->outputs.items[index]);
      btc_hash256_final(&ctx, outputs);
    }
  }

  btc_hash256_init(&ctx);

  btc_uint32_update(&ctx, tx->version);
  btc_raw_update(&ctx, prevouts, 32);
  btc_raw_update(&ctx, sequences, 32);
  btc_outpoint_update(&ctx, &input->prevout);
  btc_script_update(&ctx, prev);
  btc_int64_update(&ctx, value);
  btc_uint32_update(&ctx, input->sequence);
  btc_raw_update(&ctx, outputs, 32);
  btc_uint32_update(&ctx, tx->locktime);
  btc_uint32_update(&ctx, type);

  btc_hash256_final(&ctx, hash);
}

void
btc_tx_sighash(uint8_t *hash,
               const btc_tx_t *tx,
               size_t index,
               const btc_script_t *prev,
               int64_t value,
               unsigned int type,
               int version,
               btc_tx_cache_t *cache) {
  /* Traditional sighashing. */
  if (version == 0) {
    btc_tx_sighash_v0(hash, tx, index, prev, type);
    return;
  }

  /* Segwit sighashing. */
  if (version == 1) {
    btc_tx_sighash_v1(hash, tx, index, prev, value, type, cache);
    return;
  }

  btc_abort(); /* LCOV_EXCL_LINE */
}

int
btc_tx_verify(const btc_tx_t *tx, btc_view_t *view, uint32_t flags) {
  const btc_input_t *input;
  const btc_coin_t *coin;
  btc_tx_cache_t cache;
  size_t i;

  memset(&cache, 0, sizeof(cache));

  for (i = 0; i < tx->inputs.length; i++) {
    input = tx->inputs.items[i];
    coin = btc_view_get(view, &input->prevout);

    if (coin == NULL)
      return 0;

    if (!btc_tx_verify_input(tx, i, &coin->output, flags, &cache))
      return 0;
  }

  return 1;
}

int
btc_tx_verify_input(const btc_tx_t *tx,
                    size_t index,
                    const btc_output_t *coin,
                    uint32_t flags,
                    btc_tx_cache_t *cache) {
  const btc_input_t *input = tx->inputs.items[index];

  int ret = btc_script_verify(&input->script,
                              &input->witness,
                              &coin->script,
                              tx,
                              index,
                              coin->value,
                              flags,
                              cache);

  return ret == BTC_SCRIPT_ERR_OK;
}

int
btc_tx_sign_input(btc_tx_t *tx,
                  uint32_t index,
                  const btc_output_t *coin,
                  const uint8_t *priv,
                  unsigned int type,
                  btc_tx_cache_t *cache) {
  const btc_script_t *script = &coin->script;
  btc_input_t *input = tx->inputs.items[index];
  int64_t value = coin->value;
  btc_writer_t writer;
  btc_script_t redeem;
  btc_script_t program;
  uint8_t pub65[65];
  uint8_t pub33[33];
  uint8_t hash65[20];
  uint8_t hash33[20];
  uint8_t msg[32];
  uint8_t hash[20];
  uint8_t pub[65];
  uint8_t der[74];
  uint8_t sig[64];
  size_t der_len;
  size_t len;

  if (!btc_ecdsa_pubkey_create(pub65, priv, 0))
    return 0;

  if (!btc_ecdsa_pubkey_convert(pub33, pub65, 65, 1))
    return 0;

  if (btc_script_get_p2pk(pub, &len, script)) {
    if ((len == 33 && memcmp(pub, pub33, 33) == 0)
        || (len == 65 && memcmp(pub, pub65, 65) == 0)) {
      btc_tx_sighash(msg, tx, index, script,
                     value, type, 0, cache);

      if (!btc_ecdsa_sign(sig, NULL, msg, 32, priv))
        return 0;

      CHECK(btc_ecdsa_sig_export(der, &der_len, sig));

      der[der_len++] = type;

      btc_writer_init(&writer);
      btc_writer_push_data(&writer, der, der_len);
      btc_writer_compile(&input->script, &writer);
      btc_writer_clear(&writer);

      return 1;
    }

    return 0;
  }

  btc_ripemd160(hash65, pub65, 65);
  btc_ripemd160(hash33, pub33, 33);

  if (btc_script_get_p2pkh(hash, script)) {
    if (memcmp(hash, hash33, 20) == 0 || memcmp(hash, hash65, 20) == 0) {
      btc_tx_sighash(msg, tx, index, script,
                     value, type, 0, cache);

      if (!btc_ecdsa_sign(sig, NULL, msg, 32, priv))
        return 0;

      CHECK(btc_ecdsa_sig_export(der, &der_len, sig));

      der[der_len++] = type;

      btc_writer_init(&writer);
      btc_writer_push_data(&writer, der, der_len);

      if (memcmp(hash, hash33, 20) == 0)
        btc_writer_push_data(&writer, pub33, 33);
      else
        btc_writer_push_data(&writer, pub65, 65);

      btc_writer_compile(&input->script, &writer);
      btc_writer_clear(&writer);

      return 1;
    }

    return 0;
  }

  if (btc_script_get_p2wpkh(hash, script)) {
    if (memcmp(hash, hash33, 20) != 0)
      return 0;

    btc_script_init(&redeem);
    btc_script_set_p2pkh(&redeem, hash);

    btc_tx_sighash(msg, tx, index, &redeem,
                   value, type, 1, cache);

    btc_script_clear(&redeem);

    if (!btc_ecdsa_sign(sig, NULL, msg, 32, priv))
      return 0;

    CHECK(btc_ecdsa_sig_export(der, &der_len, sig));

    der[der_len++] = type;

    btc_stack_reset(&input->witness);
    btc_stack_push_data(&input->witness, der, der_len);
    btc_stack_push_data(&input->witness, pub33, 33);

    return 1;
  }

  if (btc_script_get_p2sh(hash, script)) {
    btc_script_init(&program);
    btc_script_set_p2wpkh(&program, hash33);
    btc_script_hash160(msg, &program);

    if (memcmp(msg, hash, 20) != 0) {
      btc_script_clear(&program);
      return 0;
    }

    btc_writer_init(&writer);
    btc_writer_push_data(&writer, program.data, program.length);
    btc_writer_compile(&input->script, &writer);
    btc_writer_clear(&writer);
    btc_script_clear(&program);

    btc_script_init(&redeem);
    btc_script_set_p2pkh(&redeem, hash33);

    btc_tx_sighash(msg, tx, index, &redeem,
                   value, type, 1, cache);

    btc_script_clear(&redeem);

    if (!btc_ecdsa_sign(sig, NULL, msg, 32, priv))
      return 0;

    CHECK(btc_ecdsa_sig_export(der, &der_len, sig));

    der[der_len++] = type;

    btc_stack_reset(&input->witness);
    btc_stack_push_data(&input->witness, der, der_len);
    btc_stack_push_data(&input->witness, pub33, 33);

    return 1;
  }

  return 0;
}

int
btc_tx_is_rbf(const btc_tx_t *tx) {
  size_t i;

  for (i = 0; i < tx->inputs.length; i++) {
    if (tx->inputs.items[i]->sequence < 0xfffffffe)
      return 1;
  }

  return 0;
}

int
btc_tx_is_final(const btc_tx_t *tx, uint32_t height, uint32_t time) {
  size_t i;

  if (tx->locktime == 0)
    return 1;

  if (tx->locktime < (tx->locktime < BTC_LOCKTIME_THRESHOLD ? height : time))
    return 1;

  for (i = 0; i < tx->inputs.length; i++) {
    if (tx->inputs.items[i]->sequence != 0xffffffff)
      return 0;
  }

  return 1;
}

int
btc_tx_verify_locktime(const btc_tx_t *tx, size_t index, uint32_t predicate) {
  static const uint32_t threshold = BTC_LOCKTIME_THRESHOLD;
  const btc_input_t *input = tx->inputs.items[index];

  /* Locktimes must be of the same type (blocks or seconds). */
  if ((tx->locktime < threshold) != (predicate < threshold))
    return 0;

  if (predicate > tx->locktime)
    return 0;

  if (input->sequence == 0xffffffff)
    return 0;

  return 1;
}

int
btc_tx_verify_sequence(const btc_tx_t *tx, size_t index, uint32_t predicate) {
  static const uint32_t disable_flag = BTC_SEQUENCE_DISABLE_FLAG;
  static const uint32_t type_flag = BTC_SEQUENCE_TYPE_FLAG;
  static const uint32_t mask = BTC_SEQUENCE_MASK;
  const btc_input_t *input = tx->inputs.items[index];

  /* For future softfork capability. */
  if (predicate & disable_flag)
    return 1;

  /* Version must be >=2. */
  if (tx->version < 2)
    return 0;

  /* Cannot use the disable flag without
     the predicate also having the disable
     flag (for future softfork capability). */
  if (input->sequence & disable_flag)
    return 0;

  /* Locktimes must be of the same type (blocks or seconds). */
  if ((input->sequence & type_flag) != (predicate & type_flag))
    return 0;

  if ((predicate & mask) > (input->sequence & mask))
    return 0;

  return 1;
}

int64_t
btc_tx_input_value(const btc_tx_t *tx, btc_view_t *view) {
  const btc_input_t *input;
  const btc_coin_t *coin;
  int64_t total = 0;
  size_t i;

  for (i = 0; i < tx->inputs.length; i++) {
    input = tx->inputs.items[i];
    coin = btc_view_get(view, &input->prevout);

    if (coin == NULL)
      return -1;

    total += coin->output.value;
  }

  return total;
}

int64_t
btc_tx_output_value(const btc_tx_t *tx) {
  int64_t total = 0;
  size_t i;

  for (i = 0; i < tx->outputs.length; i++)
    total += tx->outputs.items[i]->value;

  return total;
}

int64_t
btc_tx_fee(const btc_tx_t *tx, btc_view_t *view) {
  int64_t value = btc_tx_input_value(tx, view);

  if (value < 0)
    return -1;

  return value - btc_tx_output_value(tx);
}

int
btc_tx_legacy_sigops(const btc_tx_t *tx) {
  const btc_input_t *input;
  const btc_output_t *output;
  int total = 0;
  size_t i;

  for (i = 0; i < tx->inputs.length; i++) {
    input = tx->inputs.items[i];
    total += btc_script_sigops(&input->script, 0);
  }

  for (i = 0; i < tx->outputs.length; i++) {
    output = tx->outputs.items[i];
    total += btc_script_sigops(&output->script, 0);
  }

  return total;
}

int
btc_tx_p2sh_sigops(const btc_tx_t *tx, btc_view_t *view) {
  const btc_input_t *input;
  const btc_coin_t *coin;
  int total = 0;
  size_t i;

  if (btc_tx_is_coinbase(tx))
    return 0;

  for (i = 0; i < tx->inputs.length; i++) {
    input = tx->inputs.items[i];
    coin = btc_view_get(view, &input->prevout);

    if (coin == NULL)
      continue;

    if (!btc_script_is_p2sh(&coin->output.script))
      continue;

    total += btc_script_p2sh_sigops(&coin->output.script, &input->script);
  }

  return total;
}

int
btc_tx_witness_sigops(const btc_tx_t *tx, btc_view_t *view) {
  const btc_input_t *input;
  const btc_coin_t *coin;
  int total = 0;
  size_t i;

  if (btc_tx_is_coinbase(tx))
    return 0;

  for (i = 0; i < tx->inputs.length; i++) {
    input = tx->inputs.items[i];
    coin = btc_view_get(view, &input->prevout);

    if (coin == NULL)
      continue;

    total += btc_script_witness_sigops(&coin->output.script,
                                       &input->script,
                                       &input->witness);
  }

  return total;
}

int
btc_tx_sigops_cost(const btc_tx_t *tx, btc_view_t *view, unsigned int flags) {
  int cost = btc_tx_legacy_sigops(tx) * BTC_WITNESS_SCALE_FACTOR;

  if (flags & BTC_SCRIPT_VERIFY_P2SH)
    cost += btc_tx_p2sh_sigops(tx, view) * BTC_WITNESS_SCALE_FACTOR;

  if (flags & BTC_SCRIPT_VERIFY_WITNESS)
    cost += btc_tx_witness_sigops(tx, view);

  return cost;
}

int
btc_tx_sigops(const btc_tx_t *tx, btc_view_t *view, unsigned int flags) {
  int cost = btc_tx_sigops_cost(tx, view, flags);
  return (cost + BTC_WITNESS_SCALE_FACTOR - 1) / BTC_WITNESS_SCALE_FACTOR;
}

KHASH_SET_INIT_CONST_OUTPOINT(outpoints)

int
btc_tx_has_duplicate_inputs(const btc_tx_t *tx) {
  khash_t(outpoints) *set = kh_init(outpoints);
  const btc_input_t *input;
  size_t i;
  int ret;

  CHECK(set != NULL);

  for (i = 0; i < tx->inputs.length; i++) {
    input = tx->inputs.items[i];
    ret = -1;

    kh_put(outpoints, set, &input->prevout, &ret);

    CHECK(ret != -1);

    if (ret == 0) {
      kh_destroy(outpoints, set);
      return 1;
    }
  }

  kh_destroy(outpoints, set);

  return 0;
}

#define THROW(m, s) do { \
  if (err != NULL) {     \
    err->msg = (m);      \
    err->score = (s);    \
  }                      \
  return 0;              \
} while (0)

int
btc_tx_check_sanity(btc_verify_error_t *err, const btc_tx_t *tx) {
  const btc_input_t *input;
  const btc_output_t *output;
  int64_t total = 0;
  size_t i, size;

  if (tx->inputs.length == 0)
    THROW("bad-txns-vin-empty", 100);

  if (tx->outputs.length == 0)
    THROW("bad-txns-vout-empty", 100);

  if (btc_tx_base_size(tx) > BTC_MAX_BLOCK_SIZE)
    THROW("bad-txns-oversize", 100);

  for (i = 0; i < tx->outputs.length; i++) {
    output = tx->outputs.items[i];

    if (output->value < 0)
      THROW("bad-txns-vout-negative", 100);

    if (output->value > BTC_MAX_MONEY)
      THROW("bad-txns-vout-toolarge", 100);

    total += output->value;

    if (total < 0 || total > BTC_MAX_MONEY)
      THROW("bad-txns-txouttotal-toolarge", 100);
  }

  if (btc_tx_has_duplicate_inputs(tx))
    THROW("bad-txns-inputs-duplicate", 100);

  if (btc_tx_is_coinbase(tx)) {
    size = tx->inputs.items[0]->script.length;

    if (size < 2 || size > 100)
      THROW("bad-cb-length", 100);
  } else {
    for (i = 0; i < tx->inputs.length; i++) {
      input = tx->inputs.items[i];

      if (btc_outpoint_is_null(&input->prevout))
        THROW("bad-txns-prevout-null", 10);
    }
  }

  return 1;
}

int
btc_check_inputs(btc_verify_error_t *err,
                 const btc_tx_t *tx,
                 btc_view_t *view,
                 uint32_t height) {
  const btc_input_t *input;
  const btc_coin_t *coin;
  int64_t value, fee;
  int64_t total = 0;
  size_t i;

  for (i = 0; i < tx->inputs.length; i++) {
    input = tx->inputs.items[i];
    coin = btc_view_get(view, &input->prevout);

    if (coin == NULL)
      THROW("bad-txns-inputs-missingorspent", 0);

    if (coin->coinbase) {
      CHECK(height >= coin->height);

      if (height - coin->height < BTC_COINBASE_MATURITY)
        THROW("bad-txns-premature-spend-of-coinbase", 0);
    }

    if (coin->output.value < 0 || coin->output.value > BTC_MAX_MONEY)
      THROW("bad-txns-inputvalues-outofrange", 100);

    total += coin->output.value;

    if (total < 0 || total > BTC_MAX_MONEY)
      THROW("bad-txns-inputvalues-outofrange", 100);
  }

  /* Overflows already checked in `isSane()`. */
  value = btc_tx_output_value(tx);

  if (total < value)
    THROW("bad-txns-in-belowout", 100);

  fee = total - value;

  if (fee < 0)
    THROW("bad-txns-fee-negative", 100);

  if (fee > BTC_MAX_MONEY)
    THROW("bad-txns-fee-outofrange", 100);

  return 1;
}

#undef THROW

size_t
btc_tx_base_size(const btc_tx_t *tx) {
  size_t size = 0;

  size += 4;
  size += btc_inpvec_size(&tx->inputs);
  size += btc_outvec_size(&tx->outputs);
  size += 4;

  return size;
}

size_t
btc_tx_witness_size(const btc_tx_t *tx) {
  size_t size = 0;
  size_t i;

  if (btc_tx_has_witness(tx)) {
    size += 2;

    for (i = 0; i < tx->inputs.length; i++)
      size += btc_stack_size(&tx->inputs.items[i]->witness);
  }

  return size;
}

size_t
btc_tx_size(const btc_tx_t *tx) {
  return btc_tx_base_size(tx) + btc_tx_witness_size(tx);
}

size_t
btc_tx_weight(const btc_tx_t *tx) {
  size_t base = btc_tx_base_size(tx);
  size_t wit = btc_tx_witness_size(tx);
  return (base * BTC_WITNESS_SCALE_FACTOR) + wit;
}

size_t
btc_tx_virtual_size(const btc_tx_t *tx) {
  size_t weight = btc_tx_weight(tx);
  return (weight + BTC_WITNESS_SCALE_FACTOR - 1) / BTC_WITNESS_SCALE_FACTOR;
}

size_t
btc_tx_sigops_size(const btc_tx_t *tx, int sigops) {
  size_t weight = btc_tx_weight(tx);

  sigops *= BTC_BYTES_PER_SIGOP;

  if ((size_t)sigops > weight)
    weight = sigops;

  return (weight + BTC_WITNESS_SCALE_FACTOR - 1) / BTC_WITNESS_SCALE_FACTOR;
}

uint8_t *
btc_tx_write(uint8_t *zp, const btc_tx_t *tx) {
  int witness = btc_tx_has_witness(tx);
  size_t i;

  zp = btc_uint32_write(zp, tx->version);

  if (witness) {
    zp = btc_uint8_write(zp, 0);
    zp = btc_uint8_write(zp, 1);
  }

  zp = btc_inpvec_write(zp, &tx->inputs);
  zp = btc_outvec_write(zp, &tx->outputs);

  if (witness) {
    for (i = 0; i < tx->inputs.length; i++)
      zp = btc_stack_write(zp, &tx->inputs.items[i]->witness);
  }

  zp = btc_uint32_write(zp, tx->locktime);

  return zp;
}

int
btc_tx_read(btc_tx_t *z, const uint8_t **xp, size_t *xn) {
  uint8_t flags = 0;
  size_t i;

  if (!btc_uint32_read(&z->version, xp, xn))
    return 0;

  if (*xn >= 2 && (*xp)[0] == 0 && (*xp)[1] != 0) {
    flags = (*xp)[1];
    *xp += 2;
    *xn -= 2;
  }

  if (!btc_inpvec_read(&z->inputs, xp, xn))
    return 0;

  if (!btc_outvec_read(&z->outputs, xp, xn))
    return 0;

  if (flags & 1) {
    flags ^= 1;

    for (i = 0; i < z->inputs.length; i++) {
      if (!btc_stack_read(&z->inputs.items[i]->witness, xp, xn))
        return 0;
    }
  }

  if (flags != 0)
    return 0;

  /* We'll never be able to reserialize
     this to get the regular txid, and
     there's no way it's valid anyway. */
  if (z->inputs.length == 0 && z->outputs.length != 0)
    return 0;

  if (!btc_uint32_read(&z->locktime, xp, xn))
    return 0;

  return 1;
}

btc_coin_t *
btc_tx_coin(const btc_tx_t *tx, uint32_t index, uint32_t height) {
  btc_coin_t *coin = btc_coin_create();

  coin->version = tx->version;
  coin->height = height;
  coin->coinbase = btc_tx_is_coinbase(tx);

  btc_output_copy(&coin->output, tx->outputs.items[index]);

  return coin;
}

/*
 * Transaction Vector
 */

DEFINE_SERIALIZABLE_VECTOR(btc_txvec, btc_tx, SCOPE_EXTERN)