/**
 * @file platform_setup.h
 * @brief Idempotent USB/APN platform file setup (patch-platform-setup.sh)
 */

#ifndef PLATFORM_SETUP_H
#define PLATFORM_SETUP_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Ensure platform USB/APN config files exist and are correct.
 *
 * Idempotent: skips when patch-platform.ok exists and usbenum.ini has
 * AfterPowerLoss=1. Writes usbenum.ini, mode.cfg, connman tether config,
 * optionally fixes /etc/usbenum/usbenum.ini virtualcn=0, then marks ok.
 *
 * @return 0 on success (including skip), -1 on hard failure
 */
int platform_setup_ensure(void);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_SETUP_H */
