#!/usr/bin/env Rscript

# Install local package sources (and, optionally, their local dependencies)
# into a target library using the current R build.
#
# This is intended for CRAN-compatibility smoke testing in the experimental
# multi-threaded build, without relying on network access.

args <- commandArgs(trailingOnly = TRUE)

parse_kv <- function(args) {
  kv <- list()
  for (a in args) {
    if (!startsWith(a, "--")) next
    a <- substring(a, 3)
    if (!nzchar(a)) next
    parts <- strsplit(a, "=", fixed = TRUE)[[1]]
    key <- parts[[1]]
    val <- if (length(parts) >= 2) paste(parts[-1], collapse = "=") else ""
    kv[[key]] <- val
  }
  kv
}

kv <- parse_kv(args)
lib <- kv[["lib"]]
src_root <- kv[["src-root"]]
pkgs_str <- kv[["pkgs"]]
deps <- kv[["deps"]]

if (!nzchar(lib)) stop("missing --lib=... (target library dir)")
if (!nzchar(src_root)) stop("missing --src-root=... (directory containing package sources)")
if (!nzchar(pkgs_str)) stop("missing --pkgs=pkg1,pkg2,...")

deps <- if (!nzchar(deps)) "local" else deps
if (!deps %in% c("none", "local")) stop("invalid --deps= (use 'none' or 'local')")

pkgs <- strsplit(pkgs_str, ",", fixed = TRUE)[[1]]
pkgs <- pkgs[nzchar(pkgs)]
if (!length(pkgs)) stop("empty --pkgs=")

dir.create(lib, recursive = TRUE, showWarnings = FALSE)

desc_field <- function(desc, field) {
  x <- grep(paste0("^", field, ":"), desc, value = TRUE)
  if (!length(x)) return("")
  sub(paste0("^", field, ":[[:space:]]*"), "", x[[1]])
}

parse_deps <- function(x) {
  x <- trimws(x)
  if (!nzchar(x)) return(character())
  parts <- strsplit(x, ",", fixed = TRUE)[[1]]
  parts <- trimws(parts)
  parts <- sub("[[:space:]]*\\(.*\\)$", "", parts)
  parts <- parts[nzchar(parts)]
  # Drop "R" and any base-recommended packages that are always present.
  parts <- setdiff(parts, "R")
  parts
}

pkg_desc <- function(pkgdir) {
  readLines(file.path(pkgdir, "DESCRIPTION"), warn = FALSE)
}

pkg_deps <- function(pkgdir) {
  d <- pkg_desc(pkgdir)
  unique(c(
    parse_deps(desc_field(d, "Depends")),
    parse_deps(desc_field(d, "Imports")),
    parse_deps(desc_field(d, "LinkingTo"))
  ))
}

pkg_name <- function(pkgdir) {
  d <- pkg_desc(pkgdir)
  n <- desc_field(d, "Package")
  if (!nzchar(n)) stop("missing Package: field in ", pkgdir)
  n
}

src_pkg_dir <- function(pkg) file.path(src_root, pkg)

have_src <- function(pkg) {
  d <- src_pkg_dir(pkg)
  file.exists(file.path(d, "DESCRIPTION"))
}

installed_in_lib <- function(pkg) {
  pkg %in% rownames(installed.packages(lib.loc = lib))
}

installed_anywhere <- function(pkg) {
  pkg %in% rownames(installed.packages())
}

# Compute a dependency closure over local sources only.
todo <- unique(pkgs)
if (deps == "local") {
  i <- 1L
  while (i <= length(todo)) {
    p <- todo[[i]]
    if (!have_src(p)) stop("missing local source for package '", p, "' at ", src_pkg_dir(p))
    d <- pkg_deps(src_pkg_dir(p))
    d <- d[have_src(d)]
    todo <- unique(c(todo, d))
    i <- i + 1L
  }
}

# Copy sources into a writable tempdir before installation. Some sandboxes
# disallow writing object files into the original source tree.
scratch_root <- file.path(tempdir(), "srcpkgs")
dir.create(scratch_root, showWarnings = FALSE)

copy_src <- function(pkg) {
  dst <- file.path(scratch_root, pkg)
  if (dir.exists(dst)) unlink(dst, recursive = TRUE)
  ok <- file.copy(src_pkg_dir(pkg), scratch_root, recursive = TRUE)
  if (!isTRUE(ok)) stop("failed to copy sources for '", pkg, "' into scratch dir")
  dst
}

scratch_dirs <- setNames(lapply(todo, copy_src), todo)

# Toposort-ish install: repeatedly install any package whose dependencies are
# already installed (either in the target library or in the default library
# paths). Fail if no progress is possible.
remaining <- todo
installed_now <- character()

cat("Library:   ", normalizePath(lib, winslash = "/", mustWork = TRUE), "\n", sep = "")
cat("Src root:  ", normalizePath(src_root, winslash = "/", mustWork = TRUE), "\n", sep = "")
cat("Packages:  ", paste(pkgs, collapse = ", "), "\n", sep = "")
cat("Deps mode: ", deps, "\n", sep = "")

while (length(remaining)) {
  progressed <- FALSE
  for (p in remaining) {
    d <- pkg_deps(scratch_dirs[[p]])
    if (deps == "local") {
      # Only insist on deps that are in the local closure; others must be
      # already available in the running R.
      d <- d[d %in% todo | installed_anywhere(d)]
    }
    ok <- all(vapply(d, function(x) installed_in_lib(x) || installed_anywhere(x), logical(1)))
    if (!ok) next

    cat("\n== Installing ", p, " ==\n", sep = "")
    utils::install.packages(scratch_dirs[[p]],
                            lib = lib,
                            repos = NULL,
                            type = "source",
                            quiet = FALSE,
                            dependencies = FALSE)
    if (!requireNamespace(p, lib.loc = lib, quietly = TRUE))
      stop("installed '", p, "' but cannot load it from ", lib)
    installed_now <- c(installed_now, p)
    remaining <- setdiff(remaining, p)
    progressed <- TRUE
  }
  if (!progressed) {
    missing <- lapply(remaining, function(p) {
      d <- pkg_deps(scratch_dirs[[p]])
      d[!vapply(d, function(x) installed_in_lib(x) || installed_anywhere(x), logical(1))]
    })
    names(missing) <- remaining
    stop("cannot make progress; missing dependencies:\n",
         paste(sprintf("  %s: %s", names(missing), vapply(missing, function(x) paste(x, collapse = ", "), "")),
               collapse = "\n"))
  }
}

cat("\nOK: installed and loaded ", length(installed_now), " package(s): ",
    paste(installed_now, collapse = ", "), "\n", sep = "")

