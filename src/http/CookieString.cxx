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

#include "CookieString.hxx"
#include "Tokenizer.hxx"
#include "PTokenizer.hxx"
#include "util/StringView.hxx"
#include "AllocatorPtr.hxx"

gcc_always_inline
static constexpr bool
char_is_cookie_octet(char ch) noexcept
{
	return ch == 0x21 || (ch >= 0x23 && ch <= 0x2b) ||
		(ch >= 0x2d && ch <= 0x3a) ||
		(ch >= 0x3c && ch <= 0x5b) ||
		(ch >= 0x5d && ch <= 0x7e);
}

static void
cookie_next_unquoted_value(StringView &input, StringView &value) noexcept
{
	value.size = 0;
	value.data = input.data;

	while (value.size < input.size &&
	       char_is_cookie_octet(input[value.size]))
		++value.size;

	input.skip_front(value.size);
}

gcc_always_inline
static constexpr bool
char_is_rfc_ignorant_cookie_octet(char ch) noexcept
{
	return char_is_cookie_octet(ch) ||
		ch == ' ' || ch == ',';
}

static void
cookie_next_rfc_ignorant_value(StringView &input, StringView &value) noexcept
{
	value.size = 0;
	value.data = input.data;

	while (value.size < input.size &&
	       char_is_rfc_ignorant_cookie_octet(input[value.size]))
		++value.size;

	input.skip_front(value.size);
}

static void
cookie_next_value(AllocatorPtr alloc, StringView &input,
		  StringView &value) noexcept
{
	if (!input.empty() && input.front() == '"')
		http_next_quoted_string(alloc, input, value);
	else
		cookie_next_unquoted_value(input, value);
}

static void
cookie_next_rfc_ignorant_value(AllocatorPtr alloc, StringView &input,
			       StringView &value) noexcept
{
	if (!input.empty() && input.front() == '"')
		http_next_quoted_string(alloc, input, value);
	else
		cookie_next_rfc_ignorant_value(input, value);
}

void
cookie_next_name_value(AllocatorPtr alloc, StringView &input,
		       StringView &name, StringView &value,
		       bool rfc_ignorant) noexcept
{
	http_next_token(input, name);
	if (name.empty())
		return;

	input.StripLeft();
	if (!input.empty() && input.front() == '=') {
		input.pop_front();
		input.StripLeft();

		if (rfc_ignorant)
			cookie_next_rfc_ignorant_value(alloc, input, value);
		else
			cookie_next_value(alloc, input, value);
	} else
		value = nullptr;
}
