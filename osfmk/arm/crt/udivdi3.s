/*
 * Copyright (c) 2008 - 2009, Apple Inc. All rights reserved.<BR>
 *
 * This program and the accompanying materials
 * are licensed and made available under the terms and conditions of the BSD License
 * which accompanies this distribution.  The full text of the license may be found at
 * http://opensource.org/licenses/bsd-license.php
 *
 * THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
 * WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.
 */

#include <arm/asm_help.h>

.extern ___udivmoddi4

EnterARM(__udivdi3)
  stmfd  sp!, {r7, lr}
  add  r7, sp, #0
  sub  sp, sp, #8
  mov  ip, #0
  str  ip, [sp, #0]
  bl  ___udivmoddi4
  sub  sp, r7, #0
  ldmfd  sp!, {r7, pc}