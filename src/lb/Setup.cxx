/*
 * Copyright 2007-2021 CM4all GmbH
 * All rights reserved.
 *
 * author: Max Kellermann <mk@cm4all.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * - Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the
 * distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * FOUNDATION OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "Instance.hxx"
#include "Config.hxx"
#include "Listener.hxx"
#include "Control.hxx"
#include "util/RuntimeError.hxx"

#include "lb_features.h"
#ifdef ENABLE_CERTDB
#include "ssl/Cache.hxx"
#endif

void
LbInstance::InitAllListeners()
{
	for (const auto &i : config.listeners) {
		try {
			listeners.emplace_front(*this, i);
		} catch (...) {
			std::throw_with_nested(FormatRuntimeError("Failed to set up listener '%s'",
								  i.name.c_str()));
		}
	}
}

void
LbInstance::DeinitAllListeners() noexcept
{
	listeners.clear();
}

unsigned
LbInstance::FlushSSLSessionCache(long tm) noexcept
{
	unsigned n = 0;
	for (auto &listener : listeners)
		n += listener.FlushSSLSessionCache(tm);

#ifdef ENABLE_CERTDB
	for (auto &db : cert_dbs)
		n += db.second.FlushSessionCache(tm);
#endif

	return n;
}

void
LbInstance::InitAllControls()
{
	for (const auto &i : config.controls) {
		controls.emplace_front(*this, i);
	}
}

void
LbInstance::DeinitAllControls() noexcept
{
	controls.clear();
}

void
LbInstance::EnableAllControls() noexcept
{
	for (auto &control : controls)
		control.Enable();
}
