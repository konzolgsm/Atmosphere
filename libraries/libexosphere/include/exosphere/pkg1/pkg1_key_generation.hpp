/*
 * Copyright (c) 2018-2020 Atmosphère-NX
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#pragma once
#include <vapours.hpp>

namespace ams::pkg1 {

    enum KeyGeneration : int {

        KeyGeneration_1_0_0 = 0x00,
        KeyGeneration_3_0_0 = 0x01,
        KeyGeneration_3_0_1 = 0x02,
        KeyGeneration_4_0_0 = 0x03,
        KeyGeneration_5_0_0 = 0x04,
        KeyGeneration_6_0_0 = 0x05,
        KeyGeneration_6_2_0 = 0x06,
        KeyGeneration_7_0_0 = 0x07,
        KeyGeneration_8_1_0 = 0x08,
        KeyGeneration_9_0_0 = 0x09,
        KeyGeneration_9_1_0 = 0x0A,

        KeyGeneration_Count,

        KeyGeneration_Current = KeyGeneration_Count - 1,

        KeyGeneration_Min = 0x00,
        KeyGeneration_Max = 0x20,
    };
    static_assert(KeyGeneration_Count <= KeyGeneration_Max);

    constexpr inline const int OldMasterKeyCount       = KeyGeneration_Count - 1;
    constexpr inline const int OldDeviceMasterKeyCount = KeyGeneration_Count - KeyGeneration_4_0_0;

}
