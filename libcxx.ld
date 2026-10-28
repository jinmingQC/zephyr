/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/* libc++ detects replacements of the default allocation operators by address. */
SECTION_PROLOGUE(__lcxx_override,,SUBALIGN(4))
{
	__start___lcxx_override = .;
	KEEP(*(__lcxx_override))
	__stop___lcxx_override = .;
} GROUP_LINK_IN(ROMABLE_REGION)
