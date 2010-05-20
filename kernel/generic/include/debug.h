/*
 * Copyright (c) 2005 Martin Decky
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * - The name of the author may not be used to endorse or promote products
 *   derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/** @addtogroup genericdebug
 * @{
 */
/** @file
 */

#ifndef KERN_DEBUG_H_
#define KERN_DEBUG_H_

#include <panic.h>
#include <arch/debug.h>

#define CALLER  ((uintptr_t) __builtin_return_address(0))


#ifndef HERE

/** Current Instruction Pointer address */
#define HERE ((uintptr_t *) 0)

#endif /* HERE */


#ifdef CONFIG_DEBUG

/** Debugging ASSERT macro
 *
 * If CONFIG_DEBUG is set, the ASSERT() macro
 * evaluates expr and if it is false raises
 * kernel panic.
 *
 * @param expr Expression which is expected to be true.
 *
 */
#define ASSERT(expr) \
	if (!(expr)) { \
		panic("Assertion failed (%s), caller=%p.", #expr, CALLER); \
	}

/** Debugging verbose ASSERT macro
 *
 * If CONFIG_DEBUG is set, the ASSERT() macro
 * evaluates expr and if it is false raises
 * kernel panic. The panic message contains also
 * the supplied message.
 *
 * @param expr Expression which is expected to be true.
 * @param msg  Additional message to show (string).
 *
 */
#define ASSERT_VERBOSE(expr, msg) \
	if (!(expr)) { \
		panic("Assertion failed (%s, %s), caller=%p.", #expr, msg, CALLER); \
	}

#else /* CONFIG_DEBUG */

#define ASSERT(expr)
#define ASSERT_VERBOSE(expr, msg)

#endif /* CONFIG_DEBUG */


#ifdef CONFIG_LOG

/** Extensive logging output macro
 *
 * If CONFIG_LOG is set, the LOG() macro
 * will print whatever message is indicated plus
 * an information about the location.
 *
 */
#define LOG(format, ...) \
	printf("%s() at %s:%u: " format "\n", __func__, __FILE__, \
	    __LINE__, ##__VA_ARGS__);

/** Extensive logging execute macro
 *
 * If CONFIG_LOG is set, the LOG_EXEC() macro
 * will print an information about calling a given
 * function and call it.
 *
 */
#define LOG_EXEC(fnc) \
	{ \
		printf("%s() at %s:%u: " #fnc "\n", __func__, __FILE__, \
		    __LINE__); \
		fnc; \
	}
	
#else /* CONFOG_LOG */

#define LOG(format, ...)
#define LOG_EXEC(fnc) fnc

#endif /* CONFOG_LOG */

#endif

/** @}
 */
