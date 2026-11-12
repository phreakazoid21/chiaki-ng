// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_TAP_H
#define CHIAKI_TAP_H

#include <chiaki/controller.h>

struct AVFrame;

/**
 * Frame/input tap (doc/tap.md). When chiaki-ng is started with CHIAKI_TAP_SOCK
 * set to a socket path, it serves sampled decoded frames and the controller
 * input sent to the console to one local client over a Unix socket.
 *
 * Without CHIAKI_TAP_SOCK every function returns at once, and with no client
 * connected no frame is copied or converted. The calls never wait on the
 * client: a worker thread does the conversion and socket I/O.
 */
namespace ChiakiTap
{
	/** Start listening if CHIAKI_TAP_SOCK is set (the other calls do it too). */
	void Init();

	/** The merged controller state about to be sent to the console. */
	void OfferInput(const ChiakiControllerState &state);

	/** The latest decoded frame. It is referenced, never modified. */
	void OfferFrame(const AVFrame *frame);

	/** The stream started, at width x height, or stopped. */
	void SessionStarted(unsigned width, unsigned height);
	void SessionStopped();
}

#endif // CHIAKI_TAP_H
