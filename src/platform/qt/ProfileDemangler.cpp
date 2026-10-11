/* Copyright (c) 2026 Doomhack
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "ProfileDemangler.h"

#include <cstdlib>
#include <cstring>
#include <string_view>

#include <llvm/Demangle/Demangle.h>

size_t QGBA::demangleItanium(const char* name, size_t length, char* out, size_t size) {
	if (!size || length < 2 || name[0] != '_' || name[1] != 'Z') {
		// C names, and anything else that isn't mangled
		return 0;
	}
	char* demangled = llvm::itaniumDemangle(std::string_view(name, length));
	if (!demangled) {
		return 0;
	}
	size_t demangledLength = strlen(demangled);
	if (demangledLength >= size) {
		demangledLength = size - 1;
	}
	memcpy(out, demangled, demangledLength);
	out[demangledLength] = '\0';
	free(demangled);
	return demangledLength;
}
