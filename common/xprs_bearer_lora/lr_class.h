/**
 * @file lr_class.h
 * @brief Whose frame is this: Meshtastic's or MeshCore's?
 *
 * Only `both` mode asks. In every other mode the radio is tuned to one
 * network's modulation and sync word and the question cannot arise: what
 * arrives is that network's, and the mode row says so (docs/lora.md, "One
 * radio, four modes"). On a channel an operator has put BOTH networks
 * on, the modem can no longer answer it, so this file does.
 *
 * It must answer with ONE verdict, because both engines relay what they
 * cannot read -- that is what a router does on either network -- and both
 * add a frame to their duplicate ring before they decide anything
 * (mt_mesh.c "The repeater", mc_mesh.c "The repeater"). A frame handed to
 * the wrong engine is therefore re-aired as the wrong protocol, with the
 * wrong byte decremented, and a correctly classified copy arriving later is
 * swallowed as a duplicate. So there is no handing it to both and letting
 * the winner validate: either one of them gets it, or nobody does.
 *
 * What is here is structure, and structure alone: lengths that have to add
 * up, fields that have a legal range, a payload type that exists. No
 * decryption and no signature, so the decision is arithmetic and arithmetic
 * can be tested (test_class_host.c), and so that nothing expensive runs on
 * the bearer task (docs/lora.md rule 12). Where structure is not enough the
 * verdict is LR_CLASS_EITHER and the caller resolves it with what it has
 * and this file does not: the two XPRS unwrappings, a decrypt under
 * Meshtastic's default key, and the node tables both bridges already keep.
 */
#ifndef XPRS_LR_CLASS_H
#define XPRS_LR_CLASS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The verdict. EITHER and NEITHER are the honest answers, not failures. */
typedef enum {
    LR_CLASS_NEITHER = 0,   /**< nothing on this channel claims it: drop it */
    LR_CLASS_MT,            /**< only Meshtastic's structure fits */
    LR_CLASS_MC,            /**< only MeshCore's structure fits */
    LR_CLASS_EITHER,        /**< both fit; the caller has to settle it */
} lr_class_t;

/** The channel hashes the caller knows and this file must not compute: a
 *  hash is a hash of a name and a key, which is the bridge's business. */
typedef struct {
    uint8_t lf_hash;        /**< Meshtastic's LongFast channel hash */
    uint8_t xprs_hash;      /**< the clear `XPRS` channel's, MT_CH_HASH_XPRS */
} lr_class_in_t;

/** Could this be a Meshtastic frame? Structure only. */
bool lr_class_mt_fits(const uint8_t *frame, int len, const lr_class_in_t *in);

/** Could this be a MeshCore packet? Structure only. */
bool lr_class_mc_fits(const uint8_t *frame, int len);

/** Both answers at once, as the one verdict the caller acts on. */
lr_class_t lr_classify(const uint8_t *frame, int len, const lr_class_in_t *in);

/*
 * XPRS's own traffic, in each network's envelope. Structure again, and
 * strong structure: on the Meshtastic side the clear `XPRS` channel's hash,
 * on the MeshCore side a payload type that pins the whole first byte to
 * 0x3C-0x3F. Neither is a verdict on its own -- a frame both networks'
 * structures accept can satisfy one of these by coincidence -- but when
 * EXACTLY ONE of them holds, it is the side that claims the frame as ours,
 * and the caller uses it that way (see lr_class.h's note on EITHER).
 */
bool lr_class_mt_is_xprs(const uint8_t *frame, int len, const lr_class_in_t *in);
bool lr_class_mc_is_xprs(const uint8_t *frame, int len);

#ifdef __cplusplus
}
#endif
#endif /* XPRS_LR_CLASS_H */
