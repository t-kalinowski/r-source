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

for (pkg in pkgs) {
  ok <- TRUE
  msg <- ""
  tryCatch(
    loadNamespace(pkg),
    error = function(e) {
      ok <<- FALSE
      msg <<- conditionMessage(e)
    }
  )
  if (!ok) {
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
