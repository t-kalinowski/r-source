## Smoke test for ABI compatibility with macOS R.framework-linked binary packages.
##
## Intended usage:
## 1) Build an in-tree R with --enable-R-shlib (e.g. build-mtl-shlib)
## 2) Run tools/mtl-abi-macos.sh on that build dir
## 3) Run this script with that R, pointing at an existing binary package library.
##
## Example:
##   build-mtl-shlib/bin/R --vanilla -q -f tools/mtl-abi-smoke.R \
##     --args /Users/tomasz/Library/R/arm64/4.6/library
##

args <- commandArgs(trailingOnly = TRUE)
if (length(args) != 1L) stop("expected one arg: path to a binary package library")
lib <- args[[1L]]

.libPaths(c(lib, .libPaths()))
cat("libPaths:\n", paste(.libPaths(), collapse = "\n"), "\n", sep = "")

cat("\nLoading packages from: ", lib, "\n", sep = "")

library(digest)
cat("digest ok: ", digest("abc"), "\n", sep = "")

library(Rcpp)
cat("Rcpp ok\n")

cat("\nABI smoke ok\n")

