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

.mt_capture_env <- function(env, nms = NULL)
{
    stopifnot(is.environment(env))
    if (is.null(nms)) {
        nms <- ls(env, all.names = TRUE)
    } else {
        nms <- unique(as.character(nms))
        nms <- nms[nzchar(nms)]
        nms <- nms[vapply(nms, exists, logical(1), envir = env, inherits = FALSE)]
    }
    if (!length(nms))
        return(new.env(parent = parent.env(env)))

    active <- vapply(nms, bindingIsActive, logical(1), env = env)
    if (any(active))
        stop("background() does not support active bindings in 'env'")

    vals <- mget(nms, envir = env, inherits = FALSE)
    snap <- list2env(vals, parent = parent.env(env), hash = TRUE,
                     size = max(29L, length(vals)))

    ## Preserve lexical behavior for local closures by rebinding those that
    ## were closed over the source frame to the captured snapshot.
    for (nm in nms) {
        val <- vals[[nm]]
        if (is.function(val) && identical(environment(val), env)) {
            environment(val) <- snap
            assign(nm, val, envir = snap)
        }
    }
    snap
}

.mt_expr_capture_names <- function(expr)
{
    unique(all.names(expr, functions = TRUE, max.names = -1L, unique = TRUE))
}

.mt_fn_capture_names <- function(fn)
{
    fmls <- as.list(formals(fn))
    arg_nms <- names(fmls)
    ref <- character()
    if (!is.null(body(fn)))
        ref <- c(ref, all.names(body(fn), functions = TRUE, max.names = -1L, unique = TRUE))
    if (length(fmls)) {
        for (i in seq_along(fmls)) {
            if (!identical(fmls[[i]], quote(expr = )))
                ref <- c(ref, all.names(fmls[[i]], functions = TRUE, max.names = -1L, unique = TRUE))
        }
    }
    setdiff(unique(ref), arg_nms)
}

.mt_capture_fn <- function(fn)
{
    fn <- match.fun(fn)
    fenv <- environment(fn)
    if (!is.environment(fenv))
        return(fn)
    snap <- .mt_capture_env(fenv, .mt_fn_capture_names(fn))
    environment(fn) <- snap
    fn
}

background <- function(expr, env = parent.frame())
{
    expr <- substitute(expr)
    env <- .mt_capture_env(env, .mt_expr_capture_names(expr))
    ptr <- .Internal(mtbackground(expr, env))
    structure(
        list(expr = expr, env = env, value = quote(.mt_unresolved)),
        class = "mt_future",
        ptr = ptr
    )
}

then <- function(future, fn, ...)
{
    if (!inherits(future, "mt_future"))
        stop("'future' must be an mt_future object")
    fn <- .mt_capture_fn(fn)
    ptr <- .Internal(mtthen(future, fn, list(...)))
    structure(
        list(expr = substitute(fn), env = environment(fn), value = quote(.mt_unresolved)),
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
