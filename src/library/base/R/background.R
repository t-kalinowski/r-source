#  File src/library/base/R/background.R
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

background <- function(expr, env = parent.frame())
{
    stopifnot(is.environment(env))
    expr <- substitute(expr)
    ptr <- .Internal(mtbackground(expr, env))
    structure(
        list(expr = expr, env = env, value = quote(.mt_unresolved)),
        class = "mt_future",
        ptr = ptr
    )
}

.mt_notify_fd <- function()
{
    as.integer(.Internal(mtnotifyfd()))
}

.mt_notify_drain <- function()
{
    invisible(.Internal(mtnotifydrain()))
}

wait <- function(futures, timeout = Inf)
{
    timeout <- as.double(timeout)[1L]
    if (is.finite(timeout) && timeout < 0)
        stop("invalid 'timeout' value")
    if (!is.finite(timeout) && !is.infinite(timeout))
        stop("invalid 'timeout' value")

    one <- FALSE
    if (inherits(futures, "mt_future")) {
        one <- TRUE
        futures <- list(futures)
    } else {
        futures <- as.list(futures)
    }
    if (!length(futures))
        return(NULL)
    stopifnot(all(vapply(futures, inherits, logical(1), what = "mt_future")))

    info <- .Internal(mtwait(futures, timeout))
    if (is.null(info))
        return(NULL)

    idx <- info[["index"]]
    ok <- isTRUE(info[["ok"]])
    cancelled <- isTRUE(info[["cancelled"]])
    fut <- futures[[idx]]

    if (ok) {
        fut$value <- info[["value"]]
    } else if (cancelled) {
        fut$value <- structure(list(message = "cancelled"), class = c("mt_cancelled", "condition"))
    } else {
        fut$value <- simpleError(info[["error"]])
    }

    attr(fut, "index") <- idx
    attr(fut, "ok") <- ok
    attr(fut, "cancelled") <- cancelled
    if (!ok && !cancelled)
        attr(fut, "error") <- info[["error"]]

    if (one)
        return(fut)
    fut
}

cancel <- function(future)
{
    if (inherits(future, "mt_future"))
        return(isTRUE(.Internal(mtcancel(future))))
    if (is.list(future))
        return(vapply(future, cancel, logical(1)))
    stop("'future' must be an mt_future or list of mt_future objects")
}
