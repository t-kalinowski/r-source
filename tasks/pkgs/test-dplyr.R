#!/usr/bin/env Rscript

lib <- Sys.getenv("MTL_LIB", unset = file.path("tasks", "pkgs", "mtl-lib"))
lib <- normalizePath(lib, winslash = "/", mustWork = FALSE)

dplyr_dir <- Sys.getenv("MTL_DPLYR_DIR", unset = "/Users/tomasz/github/tidyverse/dplyr")

# Keep the run isolated from user/site libs built against a different R.
Sys.setenv(R_LIBS_USER = lib, R_LIBS_SITE = "")
Sys.setenv(R_PROFILE_USER = "/dev/null", R_ENVIRON_USER = "/dev/null")
.libPaths(c(lib, .Library))

cat("R:         ", R.version.string, "\n", sep = "")
cat("Library:   ", lib, "\n", sep = "")
cat("dplyr dir: ", dplyr_dir, "\n", sep = "")

stopifnot(requireNamespace("devtools", quietly = TRUE))
stopifnot(dir.exists(dplyr_dir))

cat("\n== Running devtools::test() ==\n")
res <- devtools::test(dplyr_dir, reporter = "summary")
print(res)

