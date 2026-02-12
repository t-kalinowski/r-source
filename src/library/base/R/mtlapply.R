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
    threads <- as.integer(getOption("mtlapply.threads", 2L))[1L]
    if (is.na(threads) || threads < 1L)
        stop("invalid value in options(\"mtlapply.threads\"): must be >= 1")
    dots <- list(...)
    if (threads == 1L)
        return(lapply(X, FUN, ...))

    n <- length(X)
    chunk <- as.integer(getOption("mtlapply.chunk_size", 50000L))[1L]
    if (is.na(chunk) || chunk < 1L)
        stop("invalid value in options(\"mtlapply.chunk_size\"): must be >= 1")
    if (n <= chunk)
        return(.Internal(mtlapply(X, FUN, dots, as.integer(threads))))

    ans <- vector("list", n)
    nms <- names(X)
    if (!is.null(nms))
        names(ans) <- nms
    starts <- seq.int(1L, n, by = chunk)
    for (start in starts) {
        end <- min(start + chunk - 1L, n)
        idx <- start:end
        ans[idx] <- .Internal(mtlapply(X[idx], FUN, dots, as.integer(threads)))
    }
    ans
}
