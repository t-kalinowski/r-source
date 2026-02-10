#!/usr/bin/env Rscript

# Bootstrap an isolated library for the experimental build and run dplyr tests.
#
# Intended usage:
#   ./build-mtl/bin/Rscript tasks/pkgs/bootstrap-and-test-dplyr.R
#
# Network access is required (installs from CRAN + GitHub).

lib <- Sys.getenv("MTL_LIB", unset = file.path("tasks", "pkgs", "mtl-lib"))
lib <- normalizePath(lib, winslash = "/", mustWork = FALSE)

dplyr_dir <- Sys.getenv("MTL_DPLYR_DIR", unset = "/Users/tomasz/github/tidyverse/dplyr")
devtools_repo <- Sys.getenv("MTL_DEVTOOLS_REPO", unset = "r-lib/devtools")

repos <- Sys.getenv("MTL_CRAN_REPO", unset = "https://cloud.r-project.org")

dir.create(lib, recursive = TRUE, showWarnings = FALSE)

# Avoid mixing in user/site libs built against a different R.
Sys.setenv(R_LIBS_USER = lib, R_LIBS_SITE = "")
.libPaths(c(lib, .Library))

options(
  repos = c(CRAN = repos),
  pkgType = "source"
)

cat("R:          ", R.version.string, "\n", sep = "")
cat("Library:    ", lib, "\n", sep = "")
cat("CRAN repo:  ", repos, "\n", sep = "")
cat("dplyr dir:  ", dplyr_dir, "\n", sep = "")
cat("devtools:   ", devtools_repo, "\n", sep = "")
cat("libPaths(): ", paste(.libPaths(), collapse = " | "), "\n", sep = "")

install_cran <- function(pkgs) {
  pkgs <- unique(pkgs)
  pkgs <- pkgs[!vapply(pkgs, requireNamespace, logical(1), quietly = TRUE)]
  if (!length(pkgs)) return(invisible(NULL))
  utils::install.packages(pkgs, lib = lib, quiet = FALSE, type = "source")
}

# 1. Ensure remotes exists (needed for GitHub + deps resolution).
install_cran("remotes")

# 2. Install devtools from GitHub (as requested).
remotes::install_github(
  devtools_repo,
  lib = lib,
  dependencies = TRUE,
  upgrade = "never",
  build = FALSE,
  quiet = FALSE
)

stopifnot(requireNamespace("devtools", quietly = TRUE))

# 3. Install dplyr dependencies for tests (CRAN).
if (!dir.exists(dplyr_dir)) stop("dplyr directory not found: ", dplyr_dir)
remotes::install_deps(
  dplyr_dir,
  lib = lib,
  dependencies = TRUE,
  upgrade = "never",
  quiet = FALSE
)

# 4. Run tests in the dplyr source directory.
cat("\n== Running devtools::test() ==\n")
res <- devtools::test(dplyr_dir, reporter = "summary")
print(res)

