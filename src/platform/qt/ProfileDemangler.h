/* Copyright (c) 2026 Doomhack
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include <cstddef>

namespace QGBA {

// Demangles an Itanium C++ ABI name, as GCC and Clang produce, for mProfileSetDemangler. Returns
// 0 for names that aren't mangled. A name too long for the buffer is cut short.
size_t demangleItanium(const char* name, size_t length, char* out, size_t size);

}
