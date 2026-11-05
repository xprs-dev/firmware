/**
 * @file fw_publisher.h
 * @brief The project's firmware publisher: the x-only public key whose
 *        signature makes an image installable on a published station, and
 *        the callsign that key derives (XPRS.md 2, 6.1.3).
 *
 * A PUBLIC key, so it lives in the tree: a station built from this tree
 * pins it as `fwkey` on first boot unless its board names another
 * (fw_secrets.h FW_DEFAULT_KEY) or its operator set one, and from then on
 * it believes firmware descriptions signed by it and nothing else
 * (docs/plan-firmware-folders.md). The private half is the maintainer's
 * own identity key and never comes near this repository or a build runner.
 */
#pragma once
#define XPRS_FW_PUBLISHER_KEY  "7756ba1e8d04b9eea1bd7e8be8e573b93add4cbfd388abe7bb1bf6673e7b47e9"
#define XPRS_FW_PUBLISHER_CALL "X1WATT"
