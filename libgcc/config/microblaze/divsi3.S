###################################-
# 
#  Copyright (C) 2009-2025 Free Software Foundation, Inc.
#
#  Contributed by Michael Eager <eager@eagercon.com>.
#
#  This file is free software; you can redistribute it and/or modify it
#  under the terms of the GNU General Public License as published by the
#  Free Software Foundation; either version 3, or (at your option) any
#  later version.
#
#  GCC is distributed in the hope that it will be useful, but WITHOUT
#  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
#  or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
#  License for more details.
#
#  Under Section 7 of GPL version 3, you are granted additional
#  permissions described in the GCC Runtime Library Exception, version
#  3.1, as published by the Free Software Foundation.
#
#  You should have received a copy of the GNU General Public License and
#  a copy of the GCC Runtime Library Exception along with this program;
#  see the files COPYING3 and COPYING.RUNTIME respectively.  If not, see
#  <http://www.gnu.org/licenses/>. 
#
#  divsi3.S
# 
#  Divide operation for 32 bit integers.
#	Input :	Dividend in Reg r5
#		Divisor in Reg r6
#	Output: Result in Reg r3
# 
#######################################
	
/* An executable stack is *not* required for these functions.  */
#ifdef __linux__
.section .note.GNU-stack,"",%progbits
.previous
#endif

	.globl	__divsi3
	.ent	__divsi3
	.type	__divsi3,@function
__divsi3:
	.frame	r1,0,r15	

	ADDIK   r1,r1,-16
	SWI     r28,r1,0
	SWI     r29,r1,4
	SWI     r30,r1,8
	SWI     r31,r1,12

	BEQI    r6,$LaDiv_By_Zero       # Div_by_Zero   # Division Error
	BEQI    r5,$LaResult_Is_Zero    # Result is Zero 
	BGEID   r5,$LaR5_Pos 
	XOR     r28,r5,r6               # Get the sign of the result
	RSUBI   r5,r5,0                 # Make r5 positive
$LaR5_Pos:
	BGEI    r6,$LaR6_Pos
	RSUBI   r6,r6,0                 # Make r6 positive
$LaR6_Pos:
	ADDIK   r30,r0,0                # Clear mod
	ADDIK   r3,r0,0                 # clear div
	ADDIK   r29,r0,32               # Initialize the loop count

        # First part try to find the first '1' in the r5
$LaDIV0: 
        BLTI    r5,$LaDIV2              # This traps r5 == 0x80000000 
$LaDIV1:
	ADD     r5,r5,r5                # left shift logical r5
	BGTID   r5,$LaDIV1       
	ADDIK   r29,r29,-1
$LaDIV2:
	ADD     r5,r5,r5                # left shift logical  r5 get the '1' into the Carry
	ADDC    r30,r30,r30             # Move that bit into the Mod register
	RSUB    r31,r6,r30              # Try to subtract (r30 a r6)
	BLTI    r31,$LaMOD_TOO_SMALL
	OR      r30,r0,r31              # Move the r31 to mod since the result was positive
	ADDIK   r3,r3,1
$LaMOD_TOO_SMALL:
	ADDIK   r29,r29,-1
	BEQi    r29,$LaLOOP_END
	ADD     r3,r3,r3                # Shift in the '1' into div
	BRI     $LaDIV2                 # Div2
$LaLOOP_END:
	BGEI    r28,$LaRETURN_HERE
	BRID    $LaRETURN_HERE
	RSUBI   r3,r3,0                 # Negate the result
$LaDiv_By_Zero:
$LaResult_Is_Zero:
	OR      r3,r0,r0 # set result to 0
$LaRETURN_HERE:
# Restore values of CSRs and that of r3 and the divisor and the dividend
	LWI     r28,r1,0
	LWI     r29,r1,4
	LWI     r30,r1,8
	LWI     r31,r1,12
	RTSD    r15,8
	ADDIK   r1,r1,16
.end __divsi3
	.size	__divsi3, . - __divsi3

