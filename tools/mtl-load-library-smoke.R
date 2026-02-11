## Load all package namespaces from one or more libraries.
##
## Usage:
##   R --vanilla -q -f tools/mtl-load-library-smoke.R --args <lib1> [<lib2> ...]

args <- commandArgs(trailingOnly = TRUE)
if (length(args) < 1L) stop("usage: <lib1> [<lib2> ...]", call. = FALSE)

libs <- normalizePath(args[file.exists(args)], winslash = "/", mustWork = FALSE)
if (!length(libs)) stop("no existing library paths supplied", call. = FALSE)

.libPaths(unique(c(libs, .libPaths())))
cat("libPaths:\n", paste(.libPaths(), collapse = "\n"), "\n", sep = "")

is_pkg_dir <- function(path) {
  file.info(path)$isdir && file.exists(file.path(path, "DESCRIPTION"))
}

pkg_dirs <- unlist(lapply(libs, function(lib) {
  entries <- list.files(lib, full.names = TRUE, all.files = FALSE, no.. = TRUE)
  entries[vapply(entries, is_pkg_dir, logical(1))]
}), use.names = FALSE)

pkgs <- sort(unique(basename(pkg_dirs)))
if (!length(pkgs)) stop("no packages found in supplied libraries", call. = FALSE)

cat("\nFound ", length(pkgs), " packages to load\n", sep = "")

skip_pkg <- character()
keep <- rep(TRUE, length(pkgs))
for (i in seq_along(pkgs)) {
  pkg <- pkgs[[i]]
  pkg_dir <- pkg_dirs[match(pkg, basename(pkg_dirs))]
  ns_path <- file.path(pkg_dir, "NAMESPACE")
  if (!file.exists(ns_path)) {
    keep[[i]] <- FALSE
    skip_pkg <- c(skip_pkg, sprintf("%s (no NAMESPACE)", pkg))
  } else if (pkg == "tcltk" && !isTRUE(capabilities("tcltk"))) {
    keep[[i]] <- FALSE
    skip_pkg <- c(skip_pkg, "tcltk (capabilities('tcltk') is FALSE)")
  }
}
pkgs <- pkgs[keep]

if (length(skip_pkg)) {
  cat("\nSkipping ", length(skip_pkg), " package(s):\n", sep = "")
  for (s in skip_pkg) cat("  - ", s, "\n", sep = "")
}

fail_pkg <- character()
fail_msg <- character()
pkg_timeout <- as.double(Sys.getenv("MTL_LOAD_TIMEOUT_SEC", "20"))
if (!is.finite(pkg_timeout) || pkg_timeout <= 0) pkg_timeout <- 20
r_bin <- file.path(R.home("bin"), "R")
if (!file.exists(r_bin))
  stop("cannot find R binary for child namespace checks: ", r_bin, call. = FALSE)

as_literal <- function(x) {
  paste(capture.output(dput(x)), collapse = "")
}

load_in_child <- function(pkg, libs, timeout_sec) {
  tf <- tempfile("mtl-load-", fileext = ".R")
  on.exit(unlink(tf), add = TRUE)
  lines <- c(
    sprintf("libs <- %s", as_literal(libs)),
    sprintf("pkg <- %s", as_literal(pkg)),
    ".libPaths(unique(c(libs, .libPaths())))",
    "suppressPackageStartupMessages(loadNamespace(pkg))",
    "cat('ok\\n')"
  )
  writeLines(lines, tf, useBytes = TRUE)
  out <- suppressWarnings(
    system2(r_bin,
            c("--vanilla", "--slave", "-f", tf),
            stdout = TRUE, stderr = TRUE,
            timeout = timeout_sec)
  )
  status <- attr(out, "status")
  if (is.null(status)) status <- 0L
  list(status = as.integer(status), output = out)
}

for (pkg in pkgs) {
  res <- load_in_child(pkg, libs, pkg_timeout)
  if (res$status != 0L) {
    msg <- if (length(res$output)) paste(res$output, collapse = " | ") else "namespace load failed"
    if (res$status == 124L)
      msg <- paste0("timeout after ", pkg_timeout, "s: ", msg)
    fail_pkg <- c(fail_pkg, pkg)
    fail_msg <- c(fail_msg, msg)
  }
}

if (length(fail_pkg)) {
  cat("\nFAILED packages:\n")
  for (i in seq_along(fail_pkg)) {
    cat(sprintf("  - %s: %s\n", fail_pkg[[i]], fail_msg[[i]]))
  }
  stop(sprintf("failed to load %d/%d packages", length(fail_pkg), length(pkgs)),
       call. = FALSE)
}

cat(sprintf("\nLoaded %d/%d packages successfully\n", length(pkgs), length(pkgs)))
