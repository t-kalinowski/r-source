## Regression smoke for reticulate startup in MTL builds.
## Targets a prior crash in:
##   reticulate::py_config()

cat("R.home(): ", R.home(), "\n", sep = "")
cat(".libPaths():\n", paste(.libPaths(), collapse = "\n"), "\n", sep = "")

suppressPackageStartupMessages(library(reticulate))
cat("reticulate namespace load ok\n")

find_pyenv_python <- function() {
  root <- path.expand("~/.pyenv/versions")
  if (!dir.exists(root)) return("")
  dirs <- list.dirs(root, recursive = FALSE, full.names = TRUE)
  if (!length(dirs)) return("")
  ## Prefer non-free-threaded builds for reticulate compatibility.
  stable <- dirs[!grepl("t", basename(dirs), fixed = TRUE)]
  if (length(stable)) dirs <- stable
  bins <- character()
  for (d in dirs) {
    bins <- c(bins,
              file.path(d, "bin", "python3"),
              file.path(d, "bin", "python"))
  }
  bins <- unique(bins[file.exists(bins)])
  if (!length(bins)) return("")
  info <- file.info(bins)
  bins <- bins[!is.na(info$size)]
  if (!length(bins)) return("")
  ord <- order(info$mtime, decreasing = TRUE)
  bins[[ord[[1L]]]]
}

if (!nzchar(Sys.getenv("RETICULATE_PYTHON", ""))) {
  py <- find_pyenv_python()
  if (!nzchar(py)) stop("no python binary found under ~/.pyenv/versions", call. = FALSE)
  Sys.setenv(RETICULATE_PYTHON = py)
}

cfg <- reticulate::py_config()
if (!is.list(cfg) || is.null(cfg$python) || !nzchar(cfg$python)) {
  stop("reticulate::py_config() did not return a valid python configuration", call. = FALSE)
}

cat("py_config ok: ", cfg$python, "\n", sep = "")
sys <- reticulate::import("sys", convert = TRUE)
exe <- tryCatch(as.character(sys$executable), error = function(e) "")
if (!nzchar(exe)) {
  stop("sys$executable is empty", call. = FALSE)
}
cat("sys.executable: ", exe, "\n", sep = "")

one_plus_one <- reticulate::py_eval("1+1", convert = TRUE)
if (!identical(as.integer(one_plus_one), 2L)) {
  stop("reticulate::py_eval('1+1') returned unexpected value", call. = FALSE)
}
cat("py_eval('1+1') ok\n")

cat("reticulate smoke ok\n")
