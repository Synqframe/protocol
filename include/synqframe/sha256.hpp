// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Synqframe

#pragma once

#include <string>
#include <string_view>

namespace synqframe {

// SHA-256 of `data` as 64 lowercase hex digits: how a layout names a file it hands out (a font's
// or a drop screen asset's sha256), so a copy fetched once can be checked and kept.
std::string sha256Hex(std::string_view data);

} // namespace synqframe
