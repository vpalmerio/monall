// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#pragma once

// The hand-written x86-64 assembly in category/vm/runtime/*.S and
// category/vm/interpreter/entry.S hard-codes the System V x86-64 argument
// registers (rdi, rsi, rdx, rcx, r8, r9). Pin every C++ function that crosses
// this boundary to that ABI so the same assembly works correctly under the
// Microsoft x64 ABI used by default on Windows. This is a no-op on platforms
// where SysV is already the default ABI.
#define MONAD_VM_SYSV_ABI __attribute__((sysv_abi))
