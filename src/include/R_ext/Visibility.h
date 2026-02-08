/*
 *  R : A Computer Language for Statistical Data Analysis
 *  Copyright (C) 2008    the R Core Team
 *
 *  This header file is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation; either version 2.1 of the License, or
 *  (at your option) any later version.
 *
 *  This file is part of R. R is distributed under the terms of the
 *  GNU General Public License, either Version 2, June 1991 or Version 3,
 *  June 2007. See doc/COPYRIGHTS for details of the copyright status of R.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with this program; if not, a copy is available at
 *  https://www.R-project.org/Licenses/
 */

/*
  Definitions controlling visibility on some platforms.

  Part of the API.
*/

#ifndef R_EXT_VISIBILITY_H_
#define R_EXT_VISIBILITY_H_

#include <Rconfig.h>

#ifdef HAVE_VISIBILITY_ATTRIBUTE
# define attribute_visible __attribute__ ((visibility ("default")))
# define attribute_hidden __attribute__ ((visibility ("hidden")))
#else
# define attribute_visible
# define attribute_hidden
#endif

/*
 * Thread-local storage support for the public C API.
 *
 * This is used by a small number of performance-sensitive inline helpers
 * (e.g. INLINE_PROTECT in Rinlinedfuns.h) and by experimental multi-threading
 * work.
 */
#ifndef R_THREAD_LOCAL
# ifdef __cplusplus
#  if __cplusplus >= 201103L
#   define R_THREAD_LOCAL thread_local
#  else
#   define R_THREAD_LOCAL /* no TLS */
#  endif
# elif defined(_MSC_VER)
#  define R_THREAD_LOCAL __declspec(thread)
# elif defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
#  define R_THREAD_LOCAL _Thread_local
# elif defined(__GNUC__) || defined(__clang__)
#  define R_THREAD_LOCAL __thread
# else
#  define R_THREAD_LOCAL /* no TLS */
# endif
#endif

#endif /* R_EXT_VISIBILITY_H_ */
