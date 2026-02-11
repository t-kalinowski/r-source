#  File src/library/base/R/mtlapply.R
#  Part of the R package, https://www.R-project.org
#
#  Copyright (C) 2026
#
#  This program is free software; you can redistribute it and/or modify
#  it under the terms of the GNU General Public License as published by
#  the Free Software Foundation; either version 2 of the License, or
#  (at your option) any later version.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  A copy of the GNU General Public License is available at
#  https://www.R-project.org/Licenses/

mtlapply <- function(X, FUN, ...)
{
    FUN <- match.fun(FUN)
    X <- as.list(X)
    threads <- as.integer(getOption("mtlapply.threads", 2L))[1L]
    if (is.na(threads) || threads < 1L)
        stop("invalid value in options(\"mtlapply.threads\"): must be >= 1")
    wrapped <- function(...) {
        tryCatch(
            FUN(...),
            error = function(e) structure(
                list(message = conditionMessage(e)),
                class = "mtlapply_internal_worker_error"
            )
        )
    }
    ans <- .Internal(mtlapply(X, wrapped, list(...), as.integer(threads)))
    err <- vapply(ans, inherits, logical(1), "mtlapply_internal_worker_error")
    if (any(err)) {
        first <- ans[[which(err)[1L]]]
        msg <- first$message
        if (!is.character(msg) || length(msg) != 1L || is.na(msg))
            msg <- "mtlapply worker error"
        invisible(.Internal(mtlpoolreset()))
        stop(msg, call. = FALSE)
    }
    ans
}
