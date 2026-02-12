## Regression smoke for installed R_ext compatibility headers used by
## prebuilt package toolchains (for example via Rcpp).

cat("R.home(): ", R.home(), "\n", sep = "")
hdr_dir <- file.path(R.home("include"), "R_ext")
cat("header dir: ", hdr_dir, "\n", sep = "")

required <- c("Callbacks.h", "PrtUtil.h")
missing <- required[!file.exists(file.path(hdr_dir, required))]
if (length(missing)) {
  stop(
    sprintf(
      "missing R_ext compatibility header(s): %s",
      paste(missing, collapse = ", ")
    ),
    call. = FALSE
  )
}

tmp <- tempfile("mtl-r-ext-", fileext = "")
dir.create(tmp)

rbin <- file.path(R.home("bin"), "R")

run_shlib <- function(src, env = character()) {
  cmd <- c("CMD", "SHLIB", src)
  out <- system2(rbin, cmd, env = env, stdout = TRUE, stderr = TRUE)
  status <- attr(out, "status")
  if (is.null(status)) status <- 0L
  if (!identical(as.integer(status), 0L)) {
    cat(paste(out, collapse = "\n"), "\n", sep = "")
    stop(sprintf("R CMD SHLIB failed for %s", basename(src)), call. = FALSE)
  }
  so <- sub("\\.(c|cc|cpp|cxx)$", .Platform$dynlib.ext, src)
  if (!file.exists(so)) {
    stop(sprintf("shared object was not created: %s", so), call. = FALSE)
  }
  dyn.load(so)
  dyn.unload(so)
}

src <- file.path(tmp, "header_smoke.c")
code <- c(
  "#include <R.h>",
  "#include <Rinternals.h>",
  "#include <R_ext/Callbacks.h>",
  "#include <R_ext/PrtUtil.h>",
  "",
  "SEXP mtl_header_smoke(SEXP x) {",
  "  Rprintf(\"header smoke loaded\\n\");",
  "  return x;",
  "}"
)
writeLines(code, src)
run_shlib(src)

if (requireNamespace("Rcpp", quietly = TRUE)) {
  rcpp_inc <- system.file("include", package = "Rcpp")
  if (nzchar(rcpp_inc)) {
    src_cpp <- file.path(tmp, "header_smoke_rcpp.cpp")
    code_cpp <- c(
      "#include <Rcpp.h>",
      "",
      "extern \"C\" SEXP mtl_header_smoke_rcpp(SEXP x) {",
      "  return x;",
      "}"
    )
    writeLines(code_cpp, src_cpp)
    run_shlib(src_cpp, env = sprintf("PKG_CPPFLAGS=-I%s", rcpp_inc))
    cat("Rcpp include-chain smoke ok\n")
  }
}

cat("R_ext header smoke ok\n")
