// Copied from the 203 Launcher (https://github.com/203-Systems/203-launcher, MIT; LICENSE and
// NOTICE.md there) by tools/launcher_ui.py: src/platform/font5x7.h at c6649a770779.
// Do not edit: change it in the Launcher and copy it again.
#pragma once

#include <cstdint>

namespace CardenzaHAL::SystemMenu::launcher {

/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 PicoTracker contributors
 *
 * Glyph rows derived from UiFont5x7::Glyph (space corrected; missing ASCII punctuation completed):
 * https://github.com/203-Systems/NullPerator/blob/45f0206d54927018ecc57c4830ca154f4af52765/sources/UI2/Text/UiFont5x7.cpp
 * 5 columns, 7 rows, MSB on the left; advance = 6 pixels.
 *
 * BSD 3-Clause License
 *
 * Copyright (c) 2018, Discodirt
 * Copyright (c) 2024, xiphonics, inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * * Neither the name of the copyright holder nor the names of its
 *   contributors may be used to endorse or promote products derived from
 *   this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

constexpr uint8_t kFont5x7[128][7] = {
    {14, 17, 1, 2, 4, 0, 4},      {14, 17, 1, 2, 4, 0, 4},
    {14, 17, 1, 2, 4, 0, 4},      {14, 17, 1, 2, 4, 0, 4},
    {14, 17, 1, 2, 4, 0, 4},      {14, 17, 1, 2, 4, 0, 4},
    {14, 17, 1, 2, 4, 0, 4},      {14, 17, 1, 2, 4, 0, 4},
    {14, 17, 1, 2, 4, 0, 4},      {14, 17, 1, 2, 4, 0, 4},
    {14, 17, 1, 2, 4, 0, 4},      {14, 17, 1, 2, 4, 0, 4},
    {14, 17, 1, 2, 4, 0, 4},      {14, 17, 1, 2, 4, 0, 4},
    {14, 17, 1, 2, 4, 0, 4},      {14, 17, 1, 2, 4, 0, 4},
    {14, 17, 1, 2, 4, 0, 4},      {14, 17, 1, 2, 4, 0, 4},
    {14, 17, 1, 2, 4, 0, 4},      {14, 17, 1, 2, 4, 0, 4},
    {14, 17, 1, 2, 4, 0, 4},      {14, 17, 1, 2, 4, 0, 4},
    {14, 17, 1, 2, 4, 0, 4},      {14, 17, 1, 2, 4, 0, 4},
    {14, 17, 1, 2, 4, 0, 4},      {14, 17, 1, 2, 4, 0, 4},
    {14, 17, 1, 2, 4, 0, 4},      {14, 17, 1, 2, 4, 0, 4},
    {14, 17, 1, 2, 4, 0, 4},      {14, 17, 1, 2, 4, 0, 4},
    {14, 17, 1, 2, 4, 0, 4},      {14, 17, 1, 2, 4, 0, 4},
    {0, 0, 0, 0, 0, 0, 0},      {4, 4, 4, 4, 4, 0, 4},
    {10, 10, 10, 0, 0, 0, 0},      {10, 31, 10, 10, 31, 10, 0},
    {4, 15, 20, 14, 5, 30, 4},      {25, 26, 4, 8, 22, 6, 0},
    {12, 18, 20, 8, 21, 18, 13},  {4, 4, 8, 0, 0, 0, 0},
    {2, 4, 8, 8, 8, 4, 2},        {8, 4, 2, 2, 2, 4, 8},
    {0, 21, 14, 31, 14, 21, 0},   {0, 4, 4, 31, 4, 4, 0},
    {0, 0, 0, 0, 6, 4, 8},        {0, 0, 0, 31, 0, 0, 0},
    {0, 0, 0, 0, 0, 6, 6},        {1, 2, 4, 8, 16, 0, 0},
    {14, 17, 19, 21, 25, 17, 14}, {4, 12, 4, 4, 4, 4, 14},
    {14, 17, 1, 2, 4, 8, 31},     {30, 1, 1, 14, 1, 1, 30},
    {2, 6, 10, 18, 31, 2, 2},     {31, 16, 16, 30, 1, 1, 30},
    {14, 16, 16, 30, 17, 17, 14}, {31, 1, 2, 4, 8, 8, 8},
    {14, 17, 17, 14, 17, 17, 14}, {14, 17, 17, 15, 1, 1, 14},
    {0, 4, 4, 0, 4, 4, 0},        {0, 4, 4, 0, 4, 4, 8},
    {1, 2, 4, 8, 4, 2, 1},        {0, 0, 31, 0, 31, 0, 0},
    {16, 8, 4, 2, 4, 8, 16},      {14, 17, 1, 2, 4, 0, 4},
    {14, 17, 23, 21, 23, 16, 14},      {14, 17, 17, 31, 17, 17, 17},
    {30, 17, 17, 30, 17, 17, 30}, {15, 16, 16, 16, 16, 16, 15},
    {30, 17, 17, 17, 17, 17, 30}, {31, 16, 16, 30, 16, 16, 31},
    {31, 16, 16, 30, 16, 16, 16}, {15, 16, 16, 23, 17, 17, 15},
    {17, 17, 17, 31, 17, 17, 17}, {14, 4, 4, 4, 4, 4, 14},
    {7, 2, 2, 2, 18, 18, 12},     {17, 18, 20, 24, 20, 18, 17},
    {16, 16, 16, 16, 16, 16, 31}, {17, 27, 21, 21, 17, 17, 17},
    {17, 25, 21, 19, 17, 17, 17}, {14, 17, 17, 17, 17, 17, 14},
    {30, 17, 17, 30, 16, 16, 16}, {14, 17, 17, 17, 21, 18, 13},
    {30, 17, 17, 30, 20, 18, 17}, {15, 16, 16, 14, 1, 1, 30},
    {31, 4, 4, 4, 4, 4, 4},       {17, 17, 17, 17, 17, 17, 14},
    {17, 17, 17, 17, 17, 10, 4},  {17, 17, 17, 21, 21, 21, 10},
    {17, 17, 10, 4, 10, 17, 17},  {17, 17, 10, 4, 4, 4, 4},
    {31, 1, 2, 4, 8, 16, 31},     {14, 8, 8, 8, 8, 8, 14},
    {16, 8, 4, 2, 1, 0, 0},      {14, 2, 2, 2, 2, 2, 14},
    {4, 10, 17, 0, 0, 0, 0},      {0, 0, 0, 0, 0, 0, 31},
    {8, 4, 2, 0, 0, 0, 0},      {0, 0, 14, 1, 15, 17, 15},
    {16, 16, 22, 25, 17, 17, 30}, {0, 0, 15, 16, 16, 16, 15},
    {1, 1, 13, 19, 17, 17, 15},   {0, 0, 14, 17, 31, 16, 15},
    {6, 9, 8, 28, 8, 8, 8},       {0, 15, 17, 17, 15, 1, 14},
    {16, 16, 22, 25, 17, 17, 17}, {4, 0, 12, 4, 4, 4, 14},
    {2, 0, 6, 2, 2, 18, 12},      {16, 16, 18, 20, 24, 20, 18},
    {12, 4, 4, 4, 4, 4, 14},      {0, 0, 26, 21, 21, 21, 21},
    {0, 0, 22, 25, 17, 17, 17},   {0, 0, 14, 17, 17, 17, 14},
    {0, 22, 25, 17, 30, 16, 16},  {0, 15, 17, 17, 15, 1, 1},
    {0, 0, 22, 25, 16, 16, 16},   {0, 0, 15, 16, 14, 1, 30},
    {8, 8, 28, 8, 8, 9, 6},       {0, 0, 17, 17, 17, 19, 13},
    {0, 0, 17, 17, 17, 10, 4},    {0, 0, 17, 17, 21, 21, 10},
    {0, 0, 17, 10, 4, 10, 17},    {0, 17, 17, 17, 15, 1, 14},
    {0, 0, 31, 2, 4, 8, 31},      {2, 4, 4, 8, 4, 4, 2},
    {4, 4, 4, 4, 4, 4, 4},      {8, 4, 4, 2, 4, 4, 8},
    {0, 0, 9, 21, 18, 0, 0},      {14, 17, 1, 2, 4, 0, 4},
};

// Descenders use the eighth cell row; keep their bitmap data and advance intact.
constexpr int font5x7YOffset(unsigned char c) {
  return c == 'g' || c == 'p' || c == 'q' || c == 'y' ? 1 : 0;
}

}  // namespace CardenzaHAL::SystemMenu::launcher
