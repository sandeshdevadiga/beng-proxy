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

#pragma once

#include "GotoConfig.hxx"
#include "ssl/Config.hxx"
#include "net/SocketConfig.hxx"

#include <string>

struct LbCertDatabaseConfig;

struct LbListenerConfig : SocketConfig {
	std::string name;

	LbGotoConfig destination;

	std::string tag;

	bool verbose_response = false;

#ifdef HAVE_NGHTTP2
	bool force_http2 = false;

	bool alpn_http2 = true;
#endif

	bool ssl = false;

	SslConfig ssl_config;

	const LbCertDatabaseConfig *cert_db = nullptr;

	explicit LbListenerConfig(const char *_name) noexcept
		:name(_name)
	{
		listen = 64;
	}

#ifdef HAVE_AVAHI
	gcc_pure
	bool HasZeroConf() const noexcept {
		return destination.HasZeroConf();
	}
#endif

	bool GetAlpnHttp2() const noexcept {
#ifdef HAVE_NGHTTP2
		return destination.GetProtocol() == LbProtocol::HTTP &&
			alpn_http2;
#else
		return false;
#endif
	}
};
